#include "app.h"
#include <objbase.h>
#include <cwctype>

namespace tec {
std::wstring historyTime(FILETIME time) {
    FILETIME local{}; SYSTEMTIME date{};
    if (!FileTimeToLocalFileTime(&time,&local) || !FileTimeToSystemTime(&local,&date)) return L"—";
    wchar_t value[40];
    swprintf_s(value,L"%04u-%02u-%02u %02u:%02u:%02u",date.wYear,date.wMonth,date.wDay,
               date.wHour,date.wMinute,date.wSecond);
    return value;
}

std::wstring historyProgram(const std::wstring& path) {
    if (path.empty()) return L"프로그램 미확인";
    const auto slash=path.find_last_of(L"\\/");
    return slash==path.npos ? path : path.substr(slash+1);
}

std::wstring historyProtocol(int protocol) {
    if (protocol==6) return L"TCP";
    if (protocol==17) return L"UDP";
    return protocol<0 ? L"" : std::to_wstring(protocol);
}

namespace {
void appendCell(std::wstring& line, const std::wstring& value) {
    if (!line.empty()) line.push_back(L',');
    line.push_back(L'"');
    // Quoting alone does not prevent spreadsheets from executing a formula.
    size_t start=0;
    while (start<value.size() && (iswspace(value[start]) || value[start]==0xfeff)) ++start;
    if (start<value.size() && wcschr(L"=+-@＝＋－＠",value[start])) line.push_back(L'\'');
    for (auto c:value) {
        if (c==L'"') line.push_back(c);
        line.push_back(c);
    }
    line.push_back(L'"');
}

DWORD writeBytes(HANDLE out, const char* data, DWORD size) {
    DWORD written=0;
    if (!WriteFile(out,data,size,&written,nullptr)) return GetLastError();
    return written==size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

DWORD writeCsv(HANDLE file, const std::vector<Row>& rows) {
    const char header[]=u8"\uFEFF최초 시각,최근 시각,관측 횟수,프로그램,실행 파일 전체 경로,목적지 IP,목적지 포트,프로토콜,참고 호스트명\r\n";
    auto error=writeBytes(file,header,sizeof(header)-1);
    if (error) return error;
    std::string buffer;
    for (const auto& row:rows) {
        const auto& a=row.first;
        std::wstring line;
        for (const auto& cell:{historyTime(a.time),historyTime(row.last),std::to_wstring(row.count),
                historyProgram(a.path),a.path,a.ip,a.port<0 ? std::wstring() : std::to_wstring(a.port),
                historyProtocol(a.protocol),row.hint})
            appendCell(line,cell);
        line+=L"\r\n";
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,line.data(),static_cast<int>(line.size()),nullptr,0,nullptr,nullptr);
        if (!size) return GetLastError();
        auto offset=buffer.size();
        buffer.resize(offset+size);
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,line.data(),static_cast<int>(line.size()),&buffer[offset],size,nullptr,nullptr))
            return GetLastError();
        if (buffer.size()>=65536) {
            error=writeBytes(file,buffer.data(),static_cast<DWORD>(buffer.size()));
            if (error) return error;
            buffer.clear();
        }
    }
    if (!buffer.empty()) error=writeBytes(file,buffer.data(),static_cast<DWORD>(buffer.size()));
    return error;
}
}

DWORD saveHistoryCsv(const std::wstring& dest,const std::vector<Row>& rows) {
    const auto slash=dest.find_last_of(L"\\/");
    if (slash==dest.npos || dest.size()<=slash+4 || _wcsicmp(dest.c_str()+dest.size()-4,L".csv") ||
        dest.find(L':',2)!=dest.npos) return ERROR_INVALID_NAME;
    GUID guid{}; wchar_t id[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,id,40)) return ERROR_GEN_FAILURE;
    const auto temp=dest.substr(0,slash+1)+L".tec-"+id+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error;
    try { error=writeCsv(file,rows); }
    catch (const std::bad_alloc&) { error=ERROR_NOT_ENOUGH_MEMORY; }
    if (!error && !FlushFileBuffers(file)) error=GetLastError();
    if (!CloseHandle(file) && !error) error=GetLastError();
    if (!error) {
        // Publish only the completed file. ReplaceFile preserves an existing destination's ACL.
        if (ReplaceFileW(dest.c_str(),temp.c_str(),nullptr,0,nullptr,nullptr)) return ERROR_SUCCESS;
        error=GetLastError();
        if (error==ERROR_FILE_NOT_FOUND) {
            if (MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_WRITE_THROUGH)) return ERROR_SUCCESS;
            error=GetLastError();
        }
    }
    DeleteFileW(temp.c_str());
    return error;
}
}
