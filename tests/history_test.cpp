#include "../src/app.h"
#include <algorithm>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
FILETIME plusSeconds(FILETIME time, unsigned long long seconds) {
    ULARGE_INTEGER n{}; n.LowPart = time.dwLowDateTime; n.HighPart = time.dwHighDateTime;
    n.QuadPart += seconds * 10000000ULL; return {n.LowPart, n.HighPart};
}
struct Folder {
    std::wstring path;
    Folder() {
        wchar_t temp[MAX_PATH]{}, name[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp) != 0, "temp path");
        require(GetTempFileNameW(temp, L"tec", 0, name) != 0, "temp name");
        DeleteFileW(name); path = name;
        require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "temp directory");
    }
    ~Folder() {
        WIN32_FIND_DATAW data{}; HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + data.cFileName).c_str()); } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
    }
};
}

void testHistory() {
    Folder folder;
    SYSTEMTIME local{}; GetLocalTime(&local);
    local.wHour = 12; local.wMinute = local.wSecond = local.wMilliseconds = 0;
    FILETIME localTime{}, base{};
    require(SystemTimeToFileTime(&local, &localTime) && LocalFileTimeToFileTime(&localTime, &base), "controlled local noon");
    std::wstring expired = folder.path + L"\\history-20000101.log";
    HANDLE file = CreateFileW(expired.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "expired fixture"); CloseHandle(file);
    {
        tec::History history(folder.path);
        require(GetFileAttributesW(expired.c_str()) == INVALID_FILE_ATTRIBUTES, "startup expires old day");
        require(history.rows().empty(), "no rows without actual blocked attempts");
        tec::Attempt a{base, L"C:\\Programs\\a.exe", L"192.0.2.2", 443, 6};
        history.push(a);
        a.time = plusSeconds(base, 60); history.push(a);
        a.time = plusSeconds(base, 61); history.push(a);
        a.path = L"C:\\Programs\\b.exe"; history.push(a);
        history.flush();
        auto rows = history.rows();
        require(rows.size() == 3, "60 second windows and full path keys");
        auto combined = std::find_if(rows.begin(), rows.end(), [](const tec::Row& r) { return r.count == 2; });
        require(combined != rows.end(), "aggregate count");
        require(CompareFileTime(&combined->first.time, &base) == 0 && CompareFileTime(&combined->last, &a.time) < 0, "earliest and latest timestamps");
        a.path = L"C:\\Programs\\a.exe"; history.push(a); history.flush();
        require(history.rows().size() == 4, "flush prevents capture-session coalescing");
        require(history.error().empty(), "history write succeeds");
    }
    {
        tec::History history(folder.path);
        require(history.rows().size() == 4, "durable reload and journal upserts");
        tec::Attempt a{plusSeconds(base, 62), L"C:\\Programs\\a.exe", L"192.0.2.2", 443, 6};
        history.push(a); history.flush();
        require(history.rows().size() == 5, "restart prevents session coalescing");
        HANDLE old = CreateFileW(expired.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(old != INVALID_HANDLE_VALUE, "maintenance fixture"); CloseHandle(old);
        history.maintain();
        require(GetFileAttributesW(expired.c_str()) == INVALID_FILE_ATTRIBUTES, "maintenance expires old day");
        require(history.error().empty(), "reload and maintenance succeed");
    }
    const int day = local.wYear * 10000 + local.wMonth * 100 + local.wDay;
    const auto log = folder.path + L"\\history-" + std::to_wstring(day) + L".log";
    file = CreateFileW(log.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "damage fixture");
    const unsigned char torn = 0xff; DWORD written = 0;
    require(WriteFile(file, &torn, 1, &written, nullptr) && written == 1, "torn journal tail"); CloseHandle(file);
    {
        tec::History history(folder.path);
        require(history.rows().size() == 5, "damaged tail keeps valid rows");
        require(!history.error().empty(), "damaged tail reports loss");
        tec::Attempt a{plusSeconds(base, 63), L"C:\\Programs\\a.exe", L"192.0.2.2", 443, 6};
        history.push(a); history.flush();
        require(history.rows().size() == 6 && !history.error().empty(), "damaged day retains unsaved row with warning");
        require(history.error().find(L"해당 날짜") != std::wstring::npos, "localized damage warning identifies day scope");
        ULARGE_INTEGER previous{}; previous.LowPart = localTime.dwLowDateTime; previous.HighPart = localTime.dwHighDateTime;
        previous.QuadPart -= 864000000000ULL;
        FILETIME previousLocal{previous.LowPart, previous.HighPart};
        require(LocalFileTimeToFileTime(&previousLocal, &a.time) != FALSE, "previous local day");
        history.push(a); history.flush();
        require(history.rows().size() == 7, "other day remains visible while one day is damaged");
    }
    {
        tec::History history(folder.path);
        require(history.rows().size() == 6, "damaged day does not block durable writes for other days");
        require(!history.error().empty(), "damage warning remains after restart");
    }
    {
        Folder edgeFolder;
        tec::History history(edgeFolder.path);
        // Calendar arithmetic uses local dates without a UTC/DST offset crossing.
        ULARGE_INTEGER calendar{}; calendar.LowPart = localTime.dwLowDateTime; calendar.HighPart = localTime.dwHighDateTime;
        calendar.QuadPart -= 6ULL * 864000000000ULL;
        FILETIME cutoffLocal{calendar.LowPart, calendar.HighPart}, cutoff{};
        require(LocalFileTimeToFileTime(&cutoffLocal, &cutoff) != FALSE, "cutoff date");
        tec::Attempt a{cutoff, L"C:\\Programs\\old.exe", L"192.0.2.3", 53, 17};
        history.push(a);
        calendar.QuadPart -= 864000000000ULL; cutoffLocal = {calendar.LowPart, calendar.HighPart};
        require(LocalFileTimeToFileTime(&cutoffLocal, &a.time) != FALSE, "expired date");
        history.push(a); history.flush();
        require(history.rows().size() == 1, "exactly seven local calendar days are retained");
        require(history.error().empty(), "retention boundary succeeds");
    }
}
