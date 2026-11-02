#include "app.h"
#include <fwpmu.h>
#include <ws2tcpip.h>
#include <rpc.h>
#include <algorithm>
#include <atomic>
#if defined(TEC_TESTING) && defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <mutex>
#include <stdexcept>

#pragma comment(lib, "Fwpuclnt.lib")
#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace tec {
namespace {
#ifdef TEC_TESTING
thread_local bool abortForTest = false, rollbackAccepted = false;
#endif
struct Prefix {
    int family = 0;
    std::array<unsigned char, 16> bytes{};
    unsigned bits = 0;
};
struct Layer {
    const GUID* key;
    int family;
    bool inbound, ale;
};
const Layer layers[] = {
    {&FWPM_LAYER_INBOUND_TRANSPORT_V4, AF_INET, true, false},
    {&FWPM_LAYER_OUTBOUND_TRANSPORT_V4, AF_INET, false, false},
    {&FWPM_LAYER_INBOUND_TRANSPORT_V6, AF_INET6, true, false},
    {&FWPM_LAYER_OUTBOUND_TRANSPORT_V6, AF_INET6, false, false},
    {&FWPM_LAYER_ALE_AUTH_CONNECT_V4, AF_INET, false, true},
    {&FWPM_LAYER_ALE_AUTH_CONNECT_V6, AF_INET6, false, true}
};

std::wstring failure(const wchar_t* operation, DWORD code) {
    return std::wstring(operation) + L" (오류 " + std::to_wstring(code) + L")";
}

bool guid(GUID& value) {
    const RPC_STATUS result = UuidCreate(&value);
    return result == RPC_S_OK || result == RPC_S_UUID_LOCAL_ONLY;
}

FWPM_FILTER_CONDITION0 number(const GUID& key, FWP_DATA_TYPE type, UINT32 value,
                             FWP_MATCH_TYPE match = FWP_MATCH_EQUAL) {
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = key;
    condition.matchType = match;
    condition.conditionValue.type = type;
    if (type == FWP_UINT8) condition.conditionValue.uint8 = static_cast<UINT8>(value);
    else if (type == FWP_UINT16) condition.conditionValue.uint16 = static_cast<UINT16>(value);
    else condition.conditionValue.uint32 = value;
    return condition;
}

UINT32 v4(const std::array<unsigned char, 16>& bytes) {
    return (UINT32(bytes[0]) << 24) | (UINT32(bytes[1]) << 16) |
           (UINT32(bytes[2]) << 8) | bytes[3];
}

Prefix mapped(const Prefix& source) {
    Prefix result;
    result.family = AF_INET6;
    result.bits = 96 + source.bits;
    result.bytes[10] = result.bytes[11] = 0xff;
    std::copy_n(source.bytes.begin(), 4, result.bytes.begin() + 12);
    return result;
}

bool readPrefixes(const std::vector<std::wstring>& source, bool hosts,
                  std::vector<Prefix>& result) {
    for (const auto& text : source) {
        Prefix prefix;
        if (!parsePrefix(text, prefix.family, prefix.bytes, prefix.bits) ||
            (prefix.family != AF_INET && prefix.family != AF_INET6) ||
            prefix.bits > (prefix.family == AF_INET ? 32u : 128u) ||
            (hosts && prefix.bits != (prefix.family == AF_INET ? 32u : 128u)) ||
            (!hosts && prefix.bits == 0)) return false;
        result.push_back(prefix);
        // Dual-stack sockets can be classified only at the IPv6 ALE layer.
        if (prefix.family == AF_INET) result.push_back(mapped(prefix));
    }
    return true;
}

std::wstring appPath(const FWP_BYTE_BLOB& blob) {
    if (!blob.data || blob.size < sizeof(wchar_t) || blob.size % sizeof(wchar_t)) return {};
    std::wstring path(blob.size / sizeof(wchar_t), L'\0');
    std::memcpy(&path[0], blob.data, blob.size);
    const auto end = path.find(L'\0');
    if (end != std::wstring::npos) path.resize(end);
    if (path.empty()) return {};
    if (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\Device\\", 8) == 0) {
        wchar_t drives[512]{};
        const DWORD size = GetLogicalDriveStringsW(512, drives);
        if (size && size < 512) {
            for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) {
                const wchar_t letter[] = {drive[0], L':', L'\0'};
                wchar_t device[32768]{};
                if (!QueryDosDeviceW(letter, device, 32768)) continue;
                for (const wchar_t* target = device; *target; target += wcslen(target) + 1) {
                    const size_t length = wcslen(target);
                    if (path.size() > length && path[length] == L'\\' &&
                        _wcsnicmp(path.c_str(), target, length) == 0)
                        return std::wstring(letter) + path.substr(length);
                }
            }
        }
    }
    if (path.compare(0, 4, L"\\??\\") == 0) path.erase(0, 4);
    return path;
}
}

struct Engine::Impl {
    mutable std::mutex control, events;
    HANDLE engine = nullptr, subscription = nullptr;
    GUID sublayer{};
    std::vector<GUID> blockKeys;
    std::vector<std::pair<UINT64, UINT16>> aleIds;
    std::function<void(Attempt)> sink;
    std::atomic<bool> running{false};
    unsigned long long generation = 0;
    bool accepting = false;
#ifdef TEC_TESTING
    std::function<DWORD(const Layer&, const FWPM_FILTER0&)> inspect;
    static void selfTest();
#endif

    static void CALLBACK event(void* context, const FWPM_NET_EVENT1* value) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (!value || value->type != FWPM_NET_EVENT_TYPE_CLASSIFY_DROP || !value->classifyDrop)
                return;
            const auto& drop = *value->classifyDrop;
            // Windows versions expose both MS_FWP_DIRECTION and FWP_DIRECTION values.
            if ((drop.msFwpDirection != 0x3901 && drop.msFwpDirection != FWP_DIRECTION_OUTBOUND) ||
                drop.isLoopback || drop.reauthReason != 0)
                return;
            std::function<void(Attempt)> callback;
            {
                std::lock_guard<std::mutex> lock(self.events);
                if (!self.accepting ||
                    std::find(self.aleIds.begin(), self.aleIds.end(),
                              std::make_pair(drop.filterId, drop.layerId)) == self.aleIds.end())
                    return;
                callback = self.sink;
            }
            if (!callback) return;
            const auto& header = value->header;
            Attempt attempt;
            attempt.time = header.timeStamp;
            if (header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) attempt.path = appPath(header.appId);
            if (header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) attempt.protocol = header.ipProtocol;
            if ((header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) &&
                (attempt.protocol == IPPROTO_TCP || attempt.protocol == IPPROTO_UDP))
                attempt.port = header.remotePort;
            if ((header.flags & (FWPM_NET_EVENT_FLAG_IP_VERSION_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)) ==
                (FWPM_NET_EVENT_FLAG_IP_VERSION_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)) {
                wchar_t address[INET6_ADDRSTRLEN]{};
                if (header.ipVersion == FWP_IP_VERSION_V4) {
                    IN_ADDR ip{};
                    ip.S_un.S_addr = htonl(header.remoteAddrV4);
                    if (InetNtopW(AF_INET, &ip, address, INET6_ADDRSTRLEN)) attempt.ip = address;
                } else if (header.ipVersion == FWP_IP_VERSION_V6) {
                    if (InetNtopW(AF_INET6, const_cast<UINT8*>(header.remoteAddrV6.byteArray16),
                                  address, INET6_ADDRSTRLEN)) attempt.ip = address;
                }
            }
            callback(std::move(attempt));
        } catch (...) {
            // Logging failures must never unwind through BFE's callback or alter enforcement.
        }
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(events);
            running = false;
            ++generation;
            accepting = false;
        }
        if (subscription) {
            FwpmNetEventUnsubscribe0(engine, subscription);
            subscription = nullptr;
        }
        if (engine) {
            FwpmEngineClose0(engine);
            engine = nullptr;
        }
        std::lock_guard<std::mutex> lock(events);
        aleIds.clear();
        blockKeys.clear();
        sink = {};
    }

    DWORD add(const Layer& layer, bool permit, std::vector<FWPM_FILTER_CONDITION0> conditions) {
        FWPM_FILTER0 filter{};
        filter.displayData.name = const_cast<wchar_t*>(permit ? L"TEC 내부 통신 허용" : L"TEC 외부 통신 차단");
        filter.layerKey = *layer.key;
        filter.subLayerKey = sublayer;
        UINT64 weight = permit ? 10 : 1;
        filter.weight.type = FWP_UINT64;
        filter.weight.uint64 = &weight;
        filter.action.type = permit ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
        filter.numFilterConditions = static_cast<UINT32>(conditions.size());
        filter.filterCondition = conditions.data();
#ifdef TEC_TESTING
        if (inspect) return inspect(layer, filter);
#endif
        if (!permit && !guid(filter.filterKey)) return ERROR_GEN_FAILURE;
        UINT64 id = 0;
        const DWORD result = FwpmFilterAdd0(engine, &filter, nullptr, &id);
        if (result == ERROR_SUCCESS && !permit) {
            std::lock_guard<std::mutex> lock(events);
            blockKeys.push_back(filter.filterKey);
            if (layer.ale) {
                FWPM_LAYER0* description = nullptr;
                const DWORD lookup = FwpmLayerGetByKey0(engine, layer.key, &description);
                if (lookup != ERROR_SUCCESS) return lookup;
                aleIds.emplace_back(id, description->layerId);
                FwpmFreeMemory0(reinterpret_cast<void**>(&description));
            }
        }
        return result;
    }

    DWORD allow(const Layer& layer, UINT64 luid, const Prefix* prefix,
                int protocol = -1, int local = -1, int remote = -1) {
        auto interface = number(FWPM_CONDITION_IP_LOCAL_INTERFACE, FWP_UINT64, 0);
        interface.conditionValue.uint64 = &luid;
        std::vector<FWPM_FILTER_CONDITION0> conditions{interface};
        FWP_V4_ADDR_AND_MASK address4{};
        FWP_V6_ADDR_AND_MASK address6{};
        if (prefix) {
            auto address = number(FWPM_CONDITION_IP_REMOTE_ADDRESS, FWP_UINT32, 0);
            if (prefix->family == AF_INET) {
                address4.addr = v4(prefix->bytes);
                address4.mask = prefix->bits == 0 ? 0 : (0xffffffffu << (32 - prefix->bits));
                address.conditionValue.type = FWP_V4_ADDR_MASK;
                address.conditionValue.v4AddrMask = &address4;
            } else {
                std::copy(prefix->bytes.begin(), prefix->bytes.end(), address6.addr);
                address6.prefixLength = static_cast<UINT8>(prefix->bits);
                address.conditionValue.type = FWP_V6_ADDR_MASK;
                address.conditionValue.v6AddrMask = &address6;
            }
            conditions.push_back(address);
        }
        if (protocol >= 0) conditions.push_back(number(FWPM_CONDITION_IP_PROTOCOL, FWP_UINT8, protocol));
        if (local >= 0) conditions.push_back(number(FWPM_CONDITION_IP_LOCAL_PORT, FWP_UINT16, local));
        if (remote >= 0) conditions.push_back(number(FWPM_CONDITION_IP_REMOTE_PORT, FWP_UINT16, remote));
        return add(layer, true, std::move(conditions));
    }

    DWORD rules(const Policy& policy, const std::vector<Prefix>& networks,
                const std::vector<Prefix>& dns, const std::vector<Prefix>& dhcp) {
        DWORD result = ERROR_SUCCESS;
        for (const auto& layer : layers) {
            if (result != ERROR_SUCCESS) break;
            result = add(layer, true, {number(FWPM_CONDITION_FLAGS, FWP_UINT32,
                         FWP_CONDITION_FLAG_IS_LOOPBACK, FWP_MATCH_FLAGS_ALL_SET)});
            for (const auto& network : networks) {
                if (result != ERROR_SUCCESS) break;
                if (network.family == layer.family) result = allow(layer, policy.luid, &network);
            }
            for (const auto& server : dns) {
                if (server.family != layer.family) continue;
                for (const int protocol : {IPPROTO_UDP, IPPROTO_TCP}) {
                    if (result == ERROR_SUCCESS) result = allow(layer, policy.luid, &server, protocol, -1, 53);
                }
            }
            for (const auto& server : dhcp) {
                if (result == ERROR_SUCCESS && server.family == layer.family)
                    result = allow(layer, policy.luid, &server, IPPROTO_UDP,
                                   layer.family == AF_INET ? 68 : 546,
                                   layer.family == AF_INET ? 67 : 547);
            }
            // Approved networks already permit unicast DHCP; only local bootstrap needs extra rules.
            const std::vector<std::wstring> bootstrap = layer.family == AF_INET ?
                (layer.inbound ? std::vector<std::wstring>{L"169.254.0.0/16"} :
                                 std::vector<std::wstring>{L"255.255.255.255/32"}) :
                (layer.inbound ? std::vector<std::wstring>{L"fe80::/10"} :
                                 std::vector<std::wstring>{L"fe80::/10", L"ff02::1:2/128"});
            for (const auto& text : bootstrap) {
                Prefix link;
                if (!parsePrefix(text, link.family, link.bytes, link.bits)) return ERROR_INVALID_DATA;
                if (result == ERROR_SUCCESS)
                    result = allow(layer, policy.luid, &link, IPPROTO_UDP,
                                   layer.family == AF_INET ? 68 : 546,
                                   layer.family == AF_INET ? 67 : 547);
            }
            if (layer.family == AF_INET6) {
                for (const auto& text : {L"fe80::/10", L"ff02::/16", L"::/128"}) {
                    Prefix link;
                    if (!parsePrefix(text, link.family, link.bytes, link.bits)) return ERROR_INVALID_DATA;
                    for (const int type : {133, 134, 135, 136}) {
                        if ((type == 133 && layer.inbound) || (type == 134 && !layer.inbound)) continue;
                        // Inbound control packets originate from link-local/unspecified, never multicast.
                        if (layer.inbound && text == std::wstring(L"ff02::/16")) continue;
                        if (!layer.inbound && text == std::wstring(L"::/128")) continue;
                        if (text == std::wstring(L"::/128") && type != 135) continue;
                        // ICMP type/code use the local/remote port fields at these layers.
                        if (result == ERROR_SUCCESS) result = allow(layer, policy.luid, &link, IPPROTO_ICMPV6, type, 0);
                    }
                }
            }
            if (result == ERROR_SUCCESS) result = add(layer, false, {});
        }
        return result;
    }
};

#ifdef TEC_TESTING
void Engine::Impl::selfTest() {
    struct Packet {
        int layer, protocol, localPort, remotePort;
        const wchar_t* address;
        UINT64 luid;
        bool loopback, expected;
    };
    const Packet packets[] = {
        {1, 6, 40000, 443, L"192.168.4.5", 123, false, true},
        {1, 6, 40000, 443, L"192.168.4.5", 124, false, false},
        {1, 6, 40000, 443, L"198.51.100.1", 123, false, false},
        {1, 17, 40000, 53, L"203.0.113.53", 123, false, true},
        {1, 6, 40000, 53, L"203.0.113.53", 123, false, true},
        {1, 6, 40000, 443, L"203.0.113.53", 123, false, false},
        {0, 17, 40000, 53, L"203.0.113.53", 123, false, true},
        {1, 17, 68, 67, L"198.51.100.1", 123, false, false},
        {1, 17, 68, 67, L"198.51.100.67", 123, false, true},
        {0, 17, 68, 67, L"198.51.100.67", 123, false, true},
        {1, 6, 68, 67, L"198.51.100.67", 123, false, false},
        {1, 17, 68, 67, L"198.51.100.67", 124, false, false},
        {1, 17, 68, 67, L"255.255.255.255", 123, false, true},
        {1, 17, 68, 67, L"255.255.255.255", 124, false, false},
        {3, 17, 546, 547, L"2001:db8:ffff::1", 123, false, false},
        {3, 17, 546, 547, L"2001:db8:ffff::547", 123, false, true},
        {3, 17, 546, 547, L"ff02::1:2", 123, false, true},
        {2, 58, 134, 0, L"fe80::1", 123, false, true},
        {2, 58, 134, 0, L"2001:db8:ffff::1", 123, false, false},
        {3, 58, 134, 0, L"ff02::1", 123, false, false},
        {3, 58, 135, 0, L"ff02::1:ff00:1", 123, false, true},
        {3, 58, 128, 0, L"ff02::1", 123, false, false},
        {2, 58, 135, 0, L"::", 123, false, true},
        {1, 6, 1000, 1001, L"198.51.100.1", 124, true, true},
        {4, 6, 40000, 443, L"192.168.4.5", 123, false, true},
        {5, 6, 40000, 443, L"::ffff:192.168.4.5", 123, false, true},
        {5, 17, 40000, 53, L"::ffff:203.0.113.53", 123, false, true},
        {5, 6, 40000, 443, L"2001:db8:ffff::1", 123, false, false}
    };
    Policy policy;
    policy.luid = 123;
    policy.networks = {L"192.168.4.0/24", L"2001:db8:4::/64"};
    policy.dns = {L"203.0.113.53"};
    policy.dhcp = {L"198.51.100.67", L"2001:db8:ffff::547"};
    std::vector<Prefix> networks, dns, dhcp;
    assert(readPrefixes(policy.networks, false, networks));
    assert(readPrefixes(policy.dns, true, dns));
    assert(readPrefixes(policy.dhcp, true, dhcp));
    assert(!readPrefixes({L"0.0.0.0/0"}, false, networks));
    assert(!readPrefixes({L"203.0.113.0/24"}, true, dns));
    Impl fake;
    bool matched[sizeof(packets) / sizeof(packets[0])]{};
    bool allowed[sizeof(packets) / sizeof(packets[0])]{};
    unsigned blocks = 0, permits = 0;
    fake.inspect = [&](const Layer& layer, const FWPM_FILTER0& filter) -> DWORD {
        const bool permit = filter.action.type == FWP_ACTION_PERMIT;
        assert(permit || filter.action.type == FWP_ACTION_BLOCK);
        assert(filter.weight.type == FWP_UINT64 && *filter.weight.uint64 == (permit ? 10u : 1u));
        if (permit) ++permits;
        else { ++blocks; assert(filter.numFilterConditions == 0); }
        bool hasLuid = false, loopback = false;
        for (UINT32 c = 0; c < filter.numFilterConditions; ++c) {
            const auto& condition = filter.filterCondition[c];
            hasLuid |= !!IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_LOCAL_INTERFACE);
            loopback |= !!IsEqualGUID(condition.fieldKey, FWPM_CONDITION_FLAGS);
        }
        assert(!permit || hasLuid || loopback);
        for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
            const auto& packet = packets[i];
            if (matched[i] || layer.key != layers[packet.layer].key) continue;
            Prefix address;
            assert(parsePrefix(packet.address, address.family, address.bytes, address.bits));
            bool match = true;
            for (UINT32 c = 0; c < filter.numFilterConditions; ++c) {
                const auto& condition = filter.filterCondition[c];
                const auto& value = condition.conditionValue;
                if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_LOCAL_INTERFACE))
                    match &= *value.uint64 == packet.luid;
                else if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_FLAGS))
                    match &= packet.loopback;
                else if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_PROTOCOL))
                    match &= value.uint8 == packet.protocol;
                else if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_LOCAL_PORT))
                    match &= value.uint16 == packet.localPort;
                else if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_REMOTE_PORT))
                    match &= value.uint16 == packet.remotePort;
                else if (IsEqualGUID(condition.fieldKey, FWPM_CONDITION_IP_REMOTE_ADDRESS)) {
                    if (value.type == FWP_V4_ADDR_MASK)
                        match &= (v4(address.bytes) & value.v4AddrMask->mask) == value.v4AddrMask->addr;
                    else {
                        for (unsigned bit = 0; bit < value.v6AddrMask->prefixLength; ++bit)
                            match &= ((address.bytes[bit / 8] ^ value.v6AddrMask->addr[bit / 8]) &
                                      (0x80u >> (bit % 8))) == 0;
                    }
                } else assert(false);
            }
            if (match) { matched[i] = true; allowed[i] = permit; }
        }
        return ERROR_SUCCESS;
    };
    assert(fake.rules(policy, networks, dns, dhcp) == ERROR_SUCCESS);
    assert(blocks == 6 && permits > 6);
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i)
        assert(matched[i] && allowed[i] == packets[i].expected);
    unsigned calls = 0;
    fake.inspect = [&](const Layer&, const FWPM_FILTER0&) -> DWORD {
        return ++calls == 3 ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    };
    assert(fake.rules(policy, networks, dns, dhcp) == ERROR_ACCESS_DENIED && calls == 3);

    std::vector<Attempt> observed;
    fake.accepting = true;
    fake.aleIds = {{UINT64(55), UINT16(48)}};
    fake.sink = [&](Attempt attempt) { observed.push_back(std::move(attempt)); };
    FWPM_NET_EVENT_CLASSIFY_DROP1 drop{};
    drop.filterId = 55;
    drop.layerId = 48;
    drop.msFwpDirection = 0x3901;
    FWPM_NET_EVENT1 value{};
    value.type = FWPM_NET_EVENT_TYPE_CLASSIFY_DROP;
    value.classifyDrop = &drop;
    auto& header = value.header;
    header.flags = FWPM_NET_EVENT_FLAG_APP_ID_SET | FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET |
                   FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET | FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                   FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET;
    wchar_t path[] = L"\\??\\C:\\test.exe";
    header.appId = {sizeof(path), reinterpret_cast<UINT8*>(path)};
    header.ipVersion = FWP_IP_VERSION_V4;
    header.remoteAddrV4 = 0xc6336401;
    header.ipProtocol = IPPROTO_TCP;
    header.remotePort = 443;
    event(&fake, &value);
    assert(observed.size() == 1 && observed.back().path == L"C:\\test.exe" &&
           observed.back().ip == L"198.51.100.1" && observed.back().port == 443);
    drop.msFwpDirection = FWP_DIRECTION_OUTBOUND;
    event(&fake, &value);
    assert(observed.size() == 2);
    for (const UINT32 direction : {UINT32(FWP_DIRECTION_INBOUND), 0x3900u, 0x3902u, 42u}) {
        drop.msFwpDirection = direction;
        event(&fake, &value);
    }
    drop.msFwpDirection = 0x3901;
    drop.filterId = 56; event(&fake, &value); drop.filterId = 55;
    drop.layerId = 49; event(&fake, &value); drop.layerId = 48;
    drop.reauthReason = 1; event(&fake, &value); drop.reauthReason = 0;
    drop.isLoopback = TRUE; event(&fake, &value); drop.isLoopback = FALSE;
    assert(observed.size() == 2);
    header.flags = 0;
    event(&fake, &value);
    assert(observed.size() == 3 && observed.back().path.empty() && observed.back().ip.empty() &&
           observed.back().protocol == -1 && observed.back().port == -1);
    fake.sink = [](Attempt) { throw 1; };
    event(&fake, &value);
    fake.close();
    event(&fake, &value);
    assert(observed.size() == 3);
    fake.accepting = true;
    fake.aleIds = {{UINT64(77), UINT16(48)}};
    fake.sink = [&](Attempt attempt) { observed.push_back(std::move(attempt)); };
    event(&fake, &value);
    assert(observed.size() == 3);
    drop.filterId = 77;
    event(&fake, &value);
    assert(observed.size() == 4);
}
#endif

Engine::Engine() : p(new Impl) {
#ifdef TEC_TESTING
    Impl::selfTest();
#elif !defined(NDEBUG)
    Prefix prefix;
    prefix.bytes = {192, 168, 5, 0};
    prefix.family = AF_INET;
    prefix.bits = 24;
    assert(v4(prefix.bytes) == 0xc0a80500u);
    const auto ipv6 = mapped(prefix);
    assert(ipv6.bits == 120 && ipv6.bytes[10] == 255 && ipv6.bytes[12] == 192);
    assert(appPath(FWP_BYTE_BLOB{}).empty());
#endif
}
Engine::~Engine() { stop(); }

bool Engine::start(const Policy& policy, std::function<void(Attempt)> callback,
                   std::wstring& error, std::wstring& logWarning) {
    std::lock_guard<std::mutex> lock(p->control);
    error.clear();
    logWarning.clear();
    if (p->running) { error = L"이미 차단 중입니다."; return false; }
    p->close();
    std::vector<Prefix> networks, dns, dhcp;
    if (!policy.luid || policy.networks.empty() ||
        !readPrefixes(policy.networks, false, networks) || !readPrefixes(policy.dns, true, dns) ||
        !readPrefixes(policy.dhcp, true, dhcp)) {
        error = L"어댑터 또는 허용 주소 설정이 올바르지 않습니다.";
        return false;
    }
    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    DWORD result = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &p->engine);
    if (result != ERROR_SUCCESS) {
        error = failure(L"필터 엔진을 열 수 없습니다. 관리자 권한을 확인하세요.", result);
        return false;
    }
    {
        std::lock_guard<std::mutex> eventLock(p->events);
        p->sink = std::move(callback);
        p->accepting = true;
    }
    FWP_VALUE0* collection = nullptr;
    result = FwpmEngineGetOption0(p->engine, FWPM_ENGINE_COLLECT_NET_EVENTS, &collection);
    const bool canLog = result == ERROR_SUCCESS && collection && collection->type == FWP_UINT32 &&
                        collection->uint32 != 0;
    if (collection) FwpmFreeMemory0(reinterpret_cast<void**>(&collection));
    if (canLog) {
        FWPM_FILTER_CONDITION0 type = number(FWPM_CONDITION_NET_EVENT_TYPE, FWP_UINT32,
                                             FWPM_NET_EVENT_TYPE_CLASSIFY_DROP);
        FWPM_NET_EVENT_ENUM_TEMPLATE0 eventTemplate{};
        eventTemplate.numFilterConditions = 1;
        eventTemplate.filterCondition = &type;
        FWPM_NET_EVENT_SUBSCRIPTION0 subscription{};
        subscription.enumTemplate = &eventTemplate;
        result = FwpmNetEventSubscribe0(p->engine, &subscription, Impl::event, p.get(), &p->subscription);
        if (result != ERROR_SUCCESS) logWarning = failure(L"차단은 가능하지만 시도 기록을 시작하지 못했습니다.", result);
    } else {
        logWarning = L"Windows 네트워크 이벤트 수집이 꺼져 있거나 조회할 수 없어 시도를 기록할 수 없습니다.";
    }
    result = FwpmTransactionBegin0(p->engine, 0);
    if (result == ERROR_SUCCESS) {
        FWPM_SUBLAYER0 sublayer{};
        if (!guid(p->sublayer)) result = ERROR_GEN_FAILURE;
        sublayer.subLayerKey = p->sublayer;
        sublayer.displayData.name = const_cast<wchar_t*>(L"TEC 외부 연결 전환");
        sublayer.weight = 0x8000;
        if (result == ERROR_SUCCESS) result = FwpmSubLayerAdd0(p->engine, &sublayer, nullptr);
        if (result == ERROR_SUCCESS) result = p->rules(policy, networks, dns, dhcp);
        if (result == ERROR_SUCCESS) {
#ifdef TEC_TESTING
            if (abortForTest) {
                result = FwpmTransactionAbort0(p->engine);
                rollbackAccepted = result == ERROR_SUCCESS;
                for (const auto& key : p->blockKeys) {
                    FWPM_FILTER0* filter = nullptr;
                    const DWORD lookup = FwpmFilterGetByKey0(p->engine, &key, &filter);
                    if (filter) FwpmFreeMemory0(reinterpret_cast<void**>(&filter));
                    rollbackAccepted &= lookup == FWP_E_FILTER_NOT_FOUND;
                }
                result = ERROR_CANCELLED;
            } else
#endif
                result = FwpmTransactionCommit0(p->engine);
        }
        else FwpmTransactionAbort0(p->engine);
    }
    if (result != ERROR_SUCCESS) {
        error = failure(L"차단 규칙을 적용하지 못했습니다. 변경 사항을 취소했습니다.", result);
        p->close();
        return false;
    }
    {
        std::lock_guard<std::mutex> eventLock(p->events);
        p->running = true;
        ++p->generation;
    }
    return true;
}

void Engine::stop() {
    std::lock_guard<std::mutex> lock(p->control);
    p->close();
}
bool Engine::active() const { return p->running.load(); }

bool Engine::healthy() const {
    for (int retry = 0; retry != 2; ++retry) {
        std::vector<GUID> keys;
        unsigned long long version;
        {
            std::lock_guard<std::mutex> lock(p->events);
            if (!p->running) return false;
            version = p->generation;
            keys = p->blockKeys;
        }
        HANDLE check = nullptr;
        DWORD result = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &check);
        if (result == ERROR_SUCCESS) {
            for (const auto& key : keys) {
                FWPM_FILTER0* filter = nullptr;
                result = FwpmFilterGetByKey0(check, &key, &filter);
                if (filter) FwpmFreeMemory0(reinterpret_cast<void**>(&filter));
                if (result != ERROR_SUCCESS) break;
            }
            FwpmEngineClose0(check);
        }
        std::lock_guard<std::mutex> lock(p->events);
        if (version != p->generation) continue;
        return result == ERROR_SUCCESS && keys.size() == 6 && p->running;
    }
    return p->running.load();
}
}

#ifdef TEC_TESTING
void testEngine() { tec::Engine engine; }
void testWfpRollback() {
    wchar_t actions[16]{};
    if (!GetEnvironmentVariableW(L"GITHUB_ACTIONS", actions, 16) || wcscmp(actions, L"true") != 0)
        throw std::runtime_error("WFP staging check is restricted to GitHub Actions");
    tec::Policy policy;
    policy.luid = 123;
    policy.networks = {L"192.0.2.0/24", L"2001:db8::/64"};
    policy.dns = {L"198.51.100.53", L"2001:db8:ffff::53"};
    policy.dhcp = {L"198.51.100.67", L"2001:db8:ffff::547"};
    tec::Engine engine;
    std::wstring error, warning;
    tec::abortForTest = true;
    tec::rollbackAccepted = false;
    engine.start(policy, {}, error, warning);
    tec::abortForTest = false;
    if (!tec::rollbackAccepted || engine.active())
        throw std::runtime_error("WFP rejected staged filters or transaction rollback failed");
}
#endif
