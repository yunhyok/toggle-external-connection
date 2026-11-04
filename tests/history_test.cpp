#include "../src/app.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <filesystem>
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

void testHistoryCsv(const std::wstring& dir) {
    const auto file=dir+L"\\내보내기.csv";
    SYSTEMTIME local{2026,11,0,4,13,20,0,0};
    FILETIME localFile{},first{};
    require(SystemTimeToFileTime(&local,&localFile) && LocalFileTimeToFileTime(&localFile,&first),"CSV local clock fixture");
    tec::Row row{{first,L"C:\\실행,폴더\\=report.exe",L"2001:db8::1",443,6},plusSeconds(first,7),3,L"a,\"b\"\r\nc"};
    const auto read=[&]() {
        std::ifstream in(std::filesystem::path(file),std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),{});
    };
    // Start with an existing longer export; replacing it must not leave trailing bytes.
    require(tec::saveHistoryCsv(file,{row,row,row})==0,"create CSV");
    tec::Row unknown{{first,L"",L"198.51.100.4",-1,-1},first,1,L"  @example"};
    require(tec::saveHistoryCsv(file,{row,unknown})==0,"replace CSV");
    const std::string expected=std::string("\xEF\xBB\xBF",3)+u8"최초 시각,최근 시각,관측 횟수,프로그램,실행 파일 전체 경로,목적지 IP,목적지 포트,프로토콜,참고 호스트명\r\n"
        u8"\"2026-11-04 13:20:00\",\"2026-11-04 13:20:07\",\"3\",\"'=report.exe\",\"C:\\실행,폴더\\=report.exe\",\"2001:db8::1\",\"443\",\"TCP\",\"a,\"\"b\"\"\r\nc\"\r\n"
        u8"\"2026-11-04 13:20:00\",\"2026-11-04 13:20:00\",\"1\",\"프로그램 미확인\",\"\",\"198.51.100.4\",\"\",\"\",\"'  @example\"\r\n";
    require(read()==expected,"CSV bytes: UTF-8 BOM, Korean, local times, escaped fields, IPv6, missing values and formulas");
    HANDLE busy=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(busy!=INVALID_HANDLE_VALUE,"lock export");
    auto error=tec::saveHistoryCsv(file,{unknown});
    CloseHandle(busy);
    require(error && read()==expected,"in-use destination is preserved");
    unknown.hint.assign(1,static_cast<wchar_t>(0xd800));
    require(tec::saveHistoryCsv(file,{row,unknown})==ERROR_NO_UNICODE_TRANSLATION && read()==expected,"partial export never replaces original");
    WIN32_FIND_DATAW entry{};
    auto leftover=FindFirstFileW((dir+L"\\.tec-*.tmp").c_str(),&entry);
    if (leftover!=INVALID_HANDLE_VALUE) FindClose(leftover);
    require(leftover==INVALID_HANDLE_VALUE,"failed export removes its staging file");
}
}

void testHistory() {
    Folder folder;
    testHistoryCsv(folder.path);
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
