#include "app.h"
#include <windns.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace tec {
namespace {
constexpr size_t queueLimit = 4096;
constexpr unsigned long long minuteTicks = 600000000ULL;
const char magic[8] = {'T','E','C','L','O','G','1',0};
using Key = std::tuple<std::wstring, std::wstring, int, int>;
unsigned long long ticks(FILETIME t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
int date(const SYSTEMTIME& s) { return s.wYear * 10000 + s.wMonth * 100 + s.wDay; }
int localDay(FILETIME t) {
    FILETIME local{}; SYSTEMTIME s{};
    return FileTimeToLocalFileTime(&t, &local) && FileTimeToSystemTime(&local, &s) ? date(s) : 0;
}
std::pair<int,int> retention() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    const int today = date(now);
    now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
    FILETIME f{}; SystemTimeToFileTime(&now, &f);
    ULARGE_INTEGER n{}; n.QuadPart = ticks(f) - 6ULL * 864000000000ULL;
    f = {n.LowPart, n.HighPart}; FileTimeToSystemTime(&f, &now);
    return {date(now), today};
}
std::wstring fileName(const std::wstring& dir, int day) {
    return dir + L"\\history-" + std::to_wstring(day) + L".log";
}
int fileDay(const wchar_t* name) {
    const std::wstring s(name);
    if (s.size() != 20 || s.substr(0, 8) != L"history-" || s.substr(16) != L".log") return 0;
    int value = 0;
    for (size_t i = 8; i < 16; ++i) { if (s[i] < L'0' || s[i] > L'9') return 0; value = value * 10 + s[i] - L'0'; }
    return value;
}
struct Handle {
    HANDLE h;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};
template<class T> void put(std::vector<unsigned char>& bytes, T value) {
    const auto* start = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), start, start + sizeof(value));
}
void putString(std::vector<unsigned char>& bytes, const std::wstring& value) {
    put(bytes, static_cast<uint32_t>(value.size()));
    const auto* start = reinterpret_cast<const unsigned char*>(value.data());
    bytes.insert(bytes.end(), start, start + value.size() * sizeof(wchar_t));
}
struct Reader {
    const std::vector<unsigned char>& bytes;
    size_t offset = 0;
    template<class T> bool get(T& value) {
        if (sizeof(value) > bytes.size() - offset) return false;
        memcpy(&value, bytes.data() + offset, sizeof(value)); offset += sizeof(value); return true;
    }
    bool string(std::wstring& value, size_t limit) {
        uint32_t length = 0;
        if (!get(length) || length > limit || length * sizeof(wchar_t) > bytes.size() - offset) return false;
        value.resize(length);
        if (length) memcpy(&value[0], bytes.data() + offset, length * sizeof(wchar_t));
        offset += length * sizeof(wchar_t);
        return value.find(L'\0') == std::wstring::npos;
    }
};
bool writeAll(HANDLE file, const void* data, size_t size) {
    DWORD written = 0;
    return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}
bool readAll(HANDLE file, void* data, DWORD size) {
    DWORD got = 0;
    return ReadFile(file, data, size, &got, nullptr) && got == size;
}
std::wstring cachedPtr(const std::wstring& ip) {
    std::wstring reverse;
    IN_ADDR a4{}; IN6_ADDR a6{};
    if (InetPtonW(AF_INET, ip.c_str(), &a4) == 1) {
        const auto* b = reinterpret_cast<const unsigned char*>(&a4);
        reverse = std::to_wstring(b[3]) + L"." + std::to_wstring(b[2]) + L"." + std::to_wstring(b[1]) + L"." + std::to_wstring(b[0]) + L".in-addr.arpa.";
    } else if (InetPtonW(AF_INET6, ip.c_str(), &a6) == 1) {
        const auto* b = reinterpret_cast<const unsigned char*>(&a6);
        const wchar_t hex[] = L"0123456789abcdef";
        for (int i = 15; i >= 0; --i) { reverse += hex[b[i] & 15]; reverse += L'.'; reverse += hex[b[i] >> 4]; reverse += L'.'; }
        reverse += L"ip6.arpa.";
    } else return {};
    PDNS_RECORDW records = nullptr;
    const auto status = DnsQuery_W(reverse.c_str(), DNS_TYPE_PTR, DNS_QUERY_NO_WIRE_QUERY | DNS_QUERY_TREAT_AS_FQDN, nullptr, &records, nullptr);
    std::wstring answer;
    if (status == ERROR_SUCCESS) {
        for (auto* r = records; r; r = r->pNext) {
            if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) { answer = r->Data.PTR.pNameHost; break; }
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    if (answer.size() > 1024) answer.resize(1024);
    return answer;
}
}

struct History::Impl {
    struct Item { unsigned long long id; int day; Row row; bool dirty; };
    std::wstring dir, failure;
    mutable std::mutex errors;
    std::atomic<unsigned long long> dropped{0};
    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) Node { SLIST_ENTRY link{}; Attempt attempt; };
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER freeNodes{}, readyNodes{};
    std::unique_ptr<Node[]> nodes{new Node[queueLimit]};
    Handle signal{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex queueMutex, stateMutex;
    std::condition_variable completed;
    std::vector<Item> items;
    std::map<Key, size_t> active;
    std::set<int> blocked;
    unsigned long long nextId = 1, requested = 0, done = 0;
    bool stopping = false, doMaintenance = false;
    int today = 0;
    std::thread worker;

    explicit Impl(std::wstring folder) : dir(std::move(folder)) {
        if (!signal) throw std::runtime_error("기록 작업 이벤트를 만들 수 없습니다.");
        InitializeSListHead(&freeNodes); InitializeSListHead(&readyNodes);
        for (size_t i = 0; i < queueLimit; ++i) InterlockedPushEntrySList(&freeNodes, &nodes[i].link);
        if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) fail(L"기록 폴더를 사용할 수 없습니다.");
        else { expire(); load(); }
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        flush(false);
        { std::lock_guard<std::mutex> lock(queueMutex); stopping = true; }
        SetEvent(signal.h); worker.join();
    }
    void fail(const std::wstring& message) { std::lock_guard<std::mutex> lock(errors); failure = message; }
    void ioFail(const std::wstring& action, int day) {
        fail(action + L" " + fileName(dir, day) + L" (Windows 오류 " + std::to_wstring(GetLastError()) + L").");
    }
    void expire() {
        const auto range = retention(); today = range.second;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((dir + L"\\history-*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name(data.cFileName);
                const bool temporary = name.size() == 24 && name.substr(20) == L".tmp";
                if (temporary) name.resize(20);
                const int day = fileDay(name.c_str());
                if (day && (temporary || day < range.first || day > range.second) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (!DeleteFileW((dir + L"\\" + data.cFileName).c_str())) ioFail(L"보관 기간이 지난 기록을 삭제할 수 없습니다:", day);
                }
            } while (FindNextFileW(find, &data));
            const DWORD result = GetLastError(); FindClose(find);
            if (result != ERROR_NO_MORE_FILES) fail(L"기록 파일 목록을 읽을 수 없습니다 (Windows 오류 " + std::to_wstring(result) + L").");
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) fail(L"기록 파일 목록을 읽을 수 없습니다.");
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& item) { return item.day < range.first || item.day > range.second; }), items.end());
        active.clear();
    }
    std::vector<unsigned char> encode(const Item& item) {
        std::vector<unsigned char> bytes;
        put(bytes, item.id); put(bytes, ticks(item.row.first.time)); put(bytes, ticks(item.row.last)); put(bytes, item.row.count);
        put(bytes, static_cast<int32_t>(item.row.first.port)); put(bytes, static_cast<int32_t>(item.row.first.protocol));
        putString(bytes, item.row.first.path); putString(bytes, item.row.first.ip); putString(bytes, item.row.hint);
        return bytes;
    }
    bool decode(const std::vector<unsigned char>& bytes, int day, Item& item) {
        Reader reader{bytes}; unsigned long long first = 0, last = 0; int32_t port = 0, protocol = 0;
        if (!reader.get(item.id) || !reader.get(first) || !reader.get(last) || !reader.get(item.row.count) || !reader.get(port) || !reader.get(protocol) ||
            !reader.string(item.row.first.path, 32768) || !reader.string(item.row.first.ip, 128) || !reader.string(item.row.hint, 1024) || reader.offset != bytes.size()) return false;
        item.row.first.time = {static_cast<DWORD>(first), static_cast<DWORD>(first >> 32)};
        item.row.last = {static_cast<DWORD>(last), static_cast<DWORD>(last >> 32)};
        item.row.first.port = port; item.row.first.protocol = protocol; item.day = day; item.dirty = false;
        return item.id && item.id != ~0ULL && item.row.count && first <= last && last - first <= minuteTicks && localDay(item.row.first.time) == day && localDay(item.row.last) == day && port >= -1 && port <= 65535 && protocol >= -1 && protocol <= 255;
    }
    void load() {
        const auto range = retention();
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((dir + L"\\history-*.log").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;
        do {
            const int day = fileDay(data.cFileName);
            if (!day || day < range.first || day > range.second || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            Handle file(CreateFileW(fileName(dir, day).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) { ioFail(L"기록을 읽을 수 없습니다:", day); blocked.insert(day); continue; }
            char header[8]{};
            bool valid = readAll(file.h, header, 8) && memcmp(header, magic, 8) == 0;
            std::map<unsigned long long, size_t> ids;
            while (valid) {
                uint32_t length = 0; DWORD got = 0;
                if (!ReadFile(file.h, &length, sizeof(length), &got, nullptr)) { valid = false; break; }
                if (!got) break;
                if (got != sizeof(length) || length < 52 || length > 70000) { valid = false; break; }
                std::vector<unsigned char> bytes(length); Item item{};
                if (!readAll(file.h, bytes.data(), length) || !decode(bytes, day, item)) { valid = false; break; }
                nextId = (std::max)(nextId, item.id + 1);
                auto existing = ids.find(item.id);
                if (existing == ids.end()) { ids[item.id] = items.size(); items.push_back(std::move(item)); }
                else items[existing->second] = std::move(item);
            }
            if (!valid) { blocked.insert(day); fail(L"기록 파일이 손상되어 읽을 수 있는 항목만 복원했습니다. 해당 날짜의 새 기록은 메모리에만 보관하며, 다른 날짜는 계속 저장합니다: " + fileName(dir, day)); }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        // Completed sessions compact once on load; runtime writes only changed rows.
        std::set<int> days;
        for (const auto& item : items) days.insert(item.day);
        for (int day : days) if (!blocked.count(day)) compact(day);
    }
    bool writeRecord(HANDLE file, const Item& item) {
        const auto bytes = encode(item);
        const uint32_t length = static_cast<uint32_t>(bytes.size());
        return writeAll(file, &length, sizeof(length)) && writeAll(file, bytes.data(), bytes.size());
    }
    void compact(int day) {
        const auto name = fileName(dir, day), temp = name + L".tmp";
        bool ok = false;
        {
            Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file) {
                ok = writeAll(file.h, magic, 8);
                for (const auto& item : items) if (ok && item.day == day) ok = writeRecord(file.h, item);
                if (ok) ok = FlushFileBuffers(file.h) != FALSE;
            }
        }
        if (ok) ok = MoveFileExW(temp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok) { ioFail(L"기록 파일을 정리할 수 없습니다:", day); DeleteFileW(temp.c_str()); }
    }
    void persist() {
        std::set<int> days;
        for (const auto& item : items) if (item.dirty) days.insert(item.day);
        for (int day : days) {
            if (blocked.count(day)) continue;
            Handle file(CreateFileW(fileName(dir, day).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) { ioFail(L"기록을 저장할 수 없습니다:", day); continue; }
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.h, &size)) { ioFail(L"기록 파일 크기를 확인할 수 없습니다:", day); continue; }
            bool ok = true;
            if (!size.QuadPart) ok = writeAll(file.h, magic, 8);
            LARGE_INTEGER zero{};
            if (ok) ok = SetFilePointerEx(file.h, zero, nullptr, FILE_END) != FALSE;
            for (const auto& item : items) if (ok && item.day == day && (item.dirty || !size.QuadPart)) ok = writeRecord(file.h, item);
            if (ok) ok = FlushFileBuffers(file.h) != FALSE;
            if (!ok) { blocked.insert(day); ioFail(L"기록 저장에 실패했습니다. 해당 날짜의 미저장 항목은 메모리에만 남습니다:", day); }
            else for (auto& item : items) if (item.day == day) item.dirty = false;
        }
    }
    void add(Attempt attempt) {
        const auto range = retention(); const int day = localDay(attempt.time);
        if (day < range.first || day > range.second) return;
        if (attempt.path.size() > 32768 || attempt.ip.size() > 128 || attempt.path.find(L'\0') != std::wstring::npos || attempt.ip.find(L'\0') != std::wstring::npos || attempt.port < -1 || attempt.port > 65535 || attempt.protocol < -1 || attempt.protocol > 255) {
            fail(L"차단 시도 정보가 올바르지 않아 기록하지 못했습니다."); return;
        }
        const Key key{attempt.path, attempt.ip, attempt.port, attempt.protocol};
        auto found = active.find(key);
        if (found != active.end()) {
            auto& item = items[found->second];
            const auto first = (std::min)(ticks(attempt.time), ticks(item.row.first.time));
            const auto last = (std::max)(ticks(attempt.time), ticks(item.row.last));
            if (item.day == day && last - first <= minuteTicks && item.row.count != ~0ULL) {
                if (ticks(attempt.time) < ticks(item.row.first.time)) item.row.first.time = attempt.time;
                if (ticks(attempt.time) > ticks(item.row.last)) item.row.last = attempt.time;
                ++item.row.count; item.dirty = true; return;
            }
        }
        if (nextId == ~0ULL) { fail(L"기록 번호를 모두 사용하여 이번 차단 시도를 저장하지 못했습니다."); return; }
        Row row{}; row.last = attempt.time; row.hint = cachedPtr(attempt.ip); row.first = std::move(attempt); row.count = 1;
        items.push_back({nextId++, day, std::move(row), true}); active[key] = items.size() - 1;
    }
    void run() {
        auto nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextSave - std::chrono::steady_clock::now()).count();
            WaitForSingleObject(signal.h, static_cast<DWORD>((std::max)(0LL, static_cast<long long>(remaining))));
            std::vector<Attempt> batch;
            unsigned long long target = 0; bool maintenance = false, stop = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                target = requested; maintenance = doMaintenance; doMaintenance = false; stop = stopping;
            }
            SLIST_ENTRY* list = InterlockedFlushSList(&readyNodes), *ordered = nullptr;
            while (list) { auto* next = list->Next; list->Next = ordered; ordered = list; list = next; }
            // Fixed nodes keep the capture callback free of allocation, locks and disk I/O.
            while (ordered) {
                auto* node = reinterpret_cast<Node*>(ordered); ordered = ordered->Next;
                try { batch.push_back(std::move(node->attempt)); } catch (...) { ++dropped; }
                InterlockedPushEntrySList(&freeNodes, &node->link);
            }
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                size_t processed = 0;
                try {
                    if (retention().second != today || maintenance) expire();
                    for (auto& attempt : batch) { add(std::move(attempt)); ++processed; }
                    if (target > done || stop || std::chrono::steady_clock::now() >= nextSave) persist();
                } catch (...) {
                    dropped += batch.size() - processed;
                    fail(L"기록 처리에 실패했습니다. 미저장 항목은 메모리에만 남아 있을 수 있습니다.");
                }
                if (target > done || stop || std::chrono::steady_clock::now() >= nextSave) nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                // A stop/flush is a capture-session boundary, even when another starts within 60 seconds.
                if (target > done) active.clear();
            }
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                done = target; completed.notify_all();
            }
            if (stop) break;
        }
    }
    void flush(bool maintenance) {
        std::unique_lock<std::mutex> lock(queueMutex);
        doMaintenance = doMaintenance || maintenance;
        const auto target = ++requested; SetEvent(signal.h);
        completed.wait(lock, [&] { return done >= target; });
    }
};

History::History(const std::wstring& dir) : p(new Impl(dir)) {}
History::~History() = default;
void History::push(Attempt attempt) {
    auto* node = reinterpret_cast<Impl::Node*>(InterlockedPopEntrySList(&p->freeNodes));
    if (!node) { ++p->dropped; return; }
    node->attempt = std::move(attempt);
    InterlockedPushEntrySList(&p->readyNodes, &node->link);
    SetEvent(p->signal.h);
}
void History::flush() { p->flush(false); }
void History::maintain() { p->flush(true); }
std::vector<Row> History::rows() {
    std::lock_guard<std::mutex> lock(p->stateMutex);
    std::vector<Row> result; result.reserve(p->items.size());
    for (const auto& item : p->items) result.push_back(item.row);
    std::sort(result.begin(), result.end(), [](const Row& a, const Row& b) { return ticks(a.last) > ticks(b.last); });
    return result;
}
std::wstring History::error() const {
    std::lock_guard<std::mutex> lock(p->errors);
    auto result = p->failure;
    const auto lost = p->dropped.load();
    if (lost) { if (!result.empty()) result += L" "; result += L"기록 대기열이 가득 찼거나 처리에 실패하여 차단 시도 " + std::to_wstring(lost) + L"건을 기록하지 못했습니다."; }
    return result;
}
}
