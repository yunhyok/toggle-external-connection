#include "app.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>

namespace tec {
namespace {
void includeFile(std::vector<std::wstring>& paths, const std::wstring& path) {
    DWORD attr=GetFileAttributesW(path.c_str());
    if (attr==INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return;
    if (std::none_of(paths.begin(),paths.end(),[&](const auto& p){return !_wcsicmp(p.c_str(),path.c_str());}))
        paths.push_back(path);
}

std::vector<std::wstring> packageRoots(const wchar_t* family) {
    // Resolve at runtime so Windows 7 can still start and use conventional installs.
    HMODULE kernel=GetModuleHandleW(L"kernel32");
    using List=LONG(WINAPI*)(PCWSTR,UINT32*,PWSTR*,UINT32*,PWSTR);
    using Locate=LONG(WINAPI*)(PCWSTR,UINT32*,PWSTR);
    auto list=reinterpret_cast<List>(GetProcAddress(kernel,"GetPackagesByPackageFamily"));
    auto locate=reinterpret_cast<Locate>(GetProcAddress(kernel,"GetPackagePathByFullName"));
    UINT32 count=0, length=0;
    std::vector<std::wstring> roots;
    if (!list || !locate || list(family,&count,nullptr,&length,nullptr)!=ERROR_INSUFFICIENT_BUFFER || !count) return roots;
    std::vector<PWSTR> names(count); std::vector<wchar_t> buf(length);
    if (list(family,&count,names.data(),&length,buf.data())!=ERROR_SUCCESS) return roots;
    for (UINT32 i=0;i<count;++i) {
        UINT32 size=0;
        if (locate(names[i],&size,nullptr)!=ERROR_INSUFFICIENT_BUFFER) continue;
        std::vector<wchar_t> path(size);
        if (locate(names[i],&size,path.data())==ERROR_SUCCESS) roots.emplace_back(path.data());
    }
    return roots;
}

void codexWorkers(const std::wstring& bin, std::vector<std::wstring>& paths) {
    includeFile(paths,bin+L"codex.exe");
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot==INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W item{}; item.dwSize=sizeof(item);
    for (BOOL ok=Process32FirstW(snapshot,&item);ok;ok=Process32NextW(snapshot,&item)) {
        if (_wcsicmp(item.szExeFile,L"codex.exe")) continue;
        HANDLE proc=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,item.th32ProcessID);
        if (!proc) continue;
        wchar_t buf[32768]; DWORD len=32768;
        bool found=QueryFullProcessImageNameW(proc,0,buf,&len)!=FALSE;
        CloseHandle(proc);
        if (!found || _wcsnicmp(buf,bin.c_str(),bin.size())) continue;
        std::wstring relative(buf+bin.size());
        if (relative.size()==26 && relative.find_first_not_of(L"0123456789abcdefABCDEF")==16 &&
            !_wcsicmp(relative.c_str()+16,L"\\codex.exe"))
            includeFile(paths,buf);
    }
    CloseHandle(snapshot);
}

void gitFiles(const std::wstring& root, std::vector<std::wstring>& paths) {
    for (auto file : {L"\\cmd\\git.exe",L"\\bin\\git.exe",L"\\mingw64\\bin\\git.exe",
              L"\\mingw64\\libexec\\git-core\\git.exe",L"\\mingw64\\libexec\\git-core\\git-remote-http.exe",
              L"\\mingw64\\libexec\\git-core\\git-remote-https.exe",L"\\mingw64\\bin\\git-lfs.exe",
              L"\\cmd\\git-lfs.exe",L"\\usr\\bin\\ssh.exe",L"\\mingw64\\bin\\git-credential-manager.exe"})
        includeFile(paths,root+file);
}
}

bool resolveApps(unsigned selected, std::vector<std::wstring>& output, std::wstring& error) {
    std::vector<std::wstring> paths;
    PWSTR dir=nullptr; std::wstring local;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&dir))) { local=dir; CoTaskMemFree(dir); }
    const wchar_t* names[]={L"Codex",L"Claude",L"Antigravity",L"Git/GitHub"};
    for (unsigned group=0;group<4;++group) {
        if (!(selected & (1u<<group))) continue;
        const size_t before=paths.size();
        if (group==0) {
            for (const auto& root : packageRoots(L"OpenAI.Codex_2p2nqsd0c76g0"))
                for (auto name : {L"\\app\\ChatGPT.exe",L"\\app\\Codex.exe",L"\\app\\resources\\codex.exe"})
                    includeFile(paths,root+name);
            if (!local.empty()) codexWorkers(local+L"\\OpenAI\\Codex\\bin\\",paths);
        } else if (group==1) {
            for (const auto& root : packageRoots(L"Claude_pzs8sxrjxfjjc"))
                includeFile(paths,root+L"\\app\\claude.exe");
        } else if (group==2 && !local.empty()) {
            for (auto relative : {L"antigravity\\Antigravity.exe",L"antigravity\\resources\\bin\\language_server.exe",
                    L"Antigravity IDE\\Antigravity IDE.exe",L"Antigravity IDE\\resources\\app\\extensions\\antigravity\\bin\\language_server_windows_x64.exe"})
                includeFile(paths,local+L"\\Programs\\"+relative);
        } else if (group==3) {
            wchar_t git[MAX_PATH]{}; DWORD len=sizeof(git);
            if (RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\GitForWindows",L"InstallPath",RRF_RT_REG_SZ,nullptr,git,&len)==ERROR_SUCCESS)
                gitFiles(git,paths);
            PWSTR pf=nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64,0,nullptr,&pf))) {
                gitFiles(std::wstring(pf)+L"\\Git",paths);
                includeFile(paths,std::wstring(pf)+L"\\GitHub CLI\\gh.exe"); CoTaskMemFree(pf);
            }
            if (!local.empty()) {
                const auto desktop=local+L"\\GitHubDesktop\\";
                WIN32_FIND_DATAW entry{};
                HANDLE found=FindFirstFileW((desktop+L"app-*").c_str(),&entry);
                if (found!=INVALID_HANDLE_VALUE) {
                    do {
                        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                        std::wstring root=desktop+entry.cFileName;
                        includeFile(paths,root+L"\\GitHubDesktop.exe");
                        gitFiles(root+L"\\resources\\app\\git",paths);
                    } while(FindNextFileW(found,&entry));
                    FindClose(found);
                }
            }
        }
        if (before==paths.size()) { error=std::wstring(names[group])+L" 실행 파일을 찾지 못했습니다. 설치 계정에서 앱을 열거나 이 예외를 해제하세요."; return false; }
    }
    output=std::move(paths); error.clear(); return true;
}
}
