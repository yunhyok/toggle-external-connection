#include "app.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>

namespace tec {
namespace {
std::wstring message(const wchar_t* what, DWORD code) {
    return std::wstring(what) + L" (Windows " + std::to_wstring(code) + L")";
}

void mask(std::array<unsigned char,16>& bytes, unsigned bits) {
    for (unsigned i = 0; i < bytes.size(); ++i) {
        if (bits >= 8) bits -= 8;
        else { bytes[i] &= static_cast<unsigned char>(0xffu << (8-bits)); bits = 0; }
    }
}

void suggest(const sockaddr* sa, int length, std::vector<std::wstring>& dest) {
    if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) return;
    const bool v4 = sa->sa_family == AF_INET;
    const unsigned full = v4 ? 32 : 128;
    if (length == 0 || length > static_cast<int>(full)) return;
    std::array<unsigned char,16> bytes{};
    if (v4) std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
    else std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, 16);
    if (std::all_of(bytes.begin(), bytes.end(), [](unsigned char b){ return b == 0; })) return;
    if (length > 0) mask(bytes, length);
    wchar_t address[INET6_ADDRSTRLEN]{};
    if (!InetNtopW(sa->sa_family, bytes.data(), address, INET6_ADDRSTRLEN)) return;
    std::wstring value(address);
    if (length > 0) value += L"/" + std::to_wstring(length);
    if (std::find(dest.begin(), dest.end(), value) == dest.end()) dest.push_back(std::move(value));
}
}

bool parsePrefix(const std::wstring& text, int& family, std::array<unsigned char,16>& bytes, unsigned& bits) {
    if (text.empty() || text.size() > 64 || text.find(L'\0') != text.npos) return false;
    const auto slash = text.find(L'/');
    const auto ip = text.substr(0, slash);
    family = ip.find(L':') == ip.npos ? AF_INET : AF_INET6;
    bytes.fill(0);
    if (InetPtonW(family, ip.c_str(), bytes.data()) != 1) return false;
    const unsigned limit = family == AF_INET ? 32 : 128;
    bits = limit;
    if (slash != text.npos) {
        if (slash + 1 == text.size()) return false;
        bits = 0;
        for (size_t i = slash+1; i < text.size(); ++i) {
            if (text[i] < L'0' || text[i] > L'9') return false;
            bits = bits*10 + (text[i]-L'0');
            if (bits > limit) return false;
        }
    }
    auto network = bytes;
    mask(network, bits);
    return bytes == network; // Reject host bits instead of silently allowing a larger network.
}

std::vector<Adapter> adapters(std::wstring& error) {
    error.clear();
    ULONG size = 16384, result = ERROR_BUFFER_OVERFLOW;
    std::vector<unsigned char> buffer;
    for (int i = 0; i < 4 && result == ERROR_BUFFER_OVERFLOW; ++i) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    std::vector<Adapter> output;
    if (result == ERROR_NO_DATA) return output;
    if (result != NO_ERROR) { error = message(L"어댑터를 읽지 못했습니다.",result); return output; }
    for (auto a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FriendlyName) continue;
        GUID guid{};
        wchar_t id[40]{};
        if (ConvertInterfaceLuidToGuid(&a->Luid, &guid) || !StringFromGUID2(guid, id, 40)) continue;
        Adapter item;
        item.guid = id; item.name = a->FriendlyName; item.suggested.luid = a->Luid.Value;
        for (auto p = a->FirstUnicastAddress; p; p = p->Next)
            if (p->DadState == IpDadStatePreferred || p->DadState == IpDadStateDeprecated)
                suggest(p->Address.lpSockaddr, p->OnLinkPrefixLength, item.suggested.networks);
        for (auto d = a->FirstDnsServerAddress; d; d = d->Next)
            suggest(d->Address.lpSockaddr, -1, item.suggested.dns);
        suggest(a->Dhcpv4Server.lpSockaddr, -1, item.suggested.dhcp);
        suggest(a->Dhcpv6Server.lpSockaddr, -1, item.suggested.dhcp);
        output.push_back(std::move(item));
    }
    return output;
}

bool validate(const Settings& value, std::wstring& error) {
    error.clear();
    GUID ignored{};
    if (value.guid.size() != 38 || value.guid.front() != L'{' || value.guid.back() != L'}' ||
        FAILED(IIDFromString(value.guid.c_str(), &ignored))) {
        error = L"내부망 어댑터를 선택하세요."; return false;
    }
    if (value.networks.empty() || value.networks.size() > 128 || value.dns.size() > 32) {
        error = L"허용할 내부 주소를 1~128개, DNS 주소를 32개 이하로 입력하세요."; return false;
    }
    for (const auto* group : {&value.networks, &value.dns}) {
        for (const auto& s : *group) {
            int family; unsigned bits; std::array<unsigned char,16> bytes{};
            if (!parsePrefix(s, family, bytes, bits) || bits == 0 ||
                (group == &value.dns && bits != (family == AF_INET ? 32u : 128u))) {
                error = L"주소를 확인하세요: " + s + L"\n단일 IP 또는 네트워크 시작 주소/CIDR를 입력하세요. DNS에는 단일 IP만 사용할 수 있습니다.";
                return false;
            }
        }
    }
    return true;
}

std::wstring storageDir(std::wstring& error) {
    auto path = installedExe(); error.clear();
    if (path.empty()) { error = L"설치 경로를 찾을 수 없습니다."; return {}; }
    const auto parent = path.substr(0, path.rfind(L'\\'));
    path = parent + L"\\Data";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        error = message(L"저장 권한을 구성하지 못했습니다.", GetLastError()); return {};
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    DWORD result = CreateDirectoryW(path.c_str(), &sa) ? ERROR_SUCCESS : GetLastError();
    LocalFree(sd);
    const auto attr = GetFileAttributesW(path.c_str());
    if ((result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) || attr == INVALID_FILE_ATTRIBUTES ||
        !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = message(L"보호된 저장 폴더를 열 수 없습니다.", result); return {};
    }
    return path;
}

bool saveSettings(const std::wstring& dir, const Settings& value, std::wstring& error) {
    if (!validate(value, error) || dir.empty()) return false;
    std::wstring contents = L"TEC1\n" + value.guid + L"\n";
    for (const auto& ip : value.networks) contents += L"net " + ip + L"\n";
    for (const auto& ip : value.dns) contents += L"dns " + ip + L"\n";
    const auto tmp = dir + L"\\settings.tmp", target = dir + L"\\settings.dat";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = message(L"설정을 쓰지 못했습니다.", GetLastError()); return false; }
    DWORD len = static_cast<DWORD>(contents.size()*sizeof(wchar_t)), written = 0;
    bool ok = WriteFile(f, contents.data(), len, &written, nullptr) && written == len && FlushFileBuffers(f);
    auto code = GetLastError(); CloseHandle(f);
    if (ok && MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    if (ok) code = GetLastError();
    DeleteFileW(tmp.c_str()); error = message(L"설정을 저장하지 못했습니다.", code); return false;
}

bool loadSettings(const std::wstring& dir, Settings& output, std::wstring& error) {
    error.clear();
    if (dir.empty()) { error = L"저장 경로가 비어 있습니다."; return false; }
    HANDLE f = CreateFileW((dir + L"\\settings.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND) error = message(L"설정을 읽지 못했습니다.", code);
        return false;
    }
    LARGE_INTEGER size{}; DWORD got = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 65536 && size.QuadPart % 2 == 0;
    std::wstring data(ok ? static_cast<size_t>(size.QuadPart/2) : 0, L'\0');
    if (ok) ok = ReadFile(f, &data[0], size.LowPart, &got, nullptr) && got == size.LowPart;
    CloseHandle(f);
    error = L"저장된 설정을 읽지 못했습니다. 내부망 설정을 다시 저장하세요.";
    if (!ok || data.compare(0,5,L"TEC1\n") || data.back() != L'\n') return false;
    Settings value;
    size_t begin = 5, end = data.find(L'\n', begin);
    if (end == data.npos) return false;
    value.guid = data.substr(begin, end-begin);
    for (begin=end+1; begin < data.size(); begin=end+1) {
        end = data.find(L'\n', begin);
        auto line = data.substr(begin,end-begin);
        if (line.compare(0,4,L"net ") == 0) value.networks.push_back(line.substr(4));
        else if (line.compare(0,4,L"dns ") == 0) value.dns.push_back(line.substr(4));
        else return false;
    }
    if (!validate(value,error)) return false;
    output = std::move(value); return true;
}
}
