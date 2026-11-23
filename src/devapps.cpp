#include "app.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>

namespace tec {
namespace {
std::wstring trimBackslashes(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool safeDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

void includeFile(std::vector<std::wstring>& paths, const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
    if (std::none_of(paths.begin(), paths.end(), [&](const auto& p) { return !_wcsicmp(p.c_str(), path.c_str()); }))
        paths.push_back(path);
}

void addUniqueRoot(std::vector<std::wstring>& roots, std::wstring root) {
    root = trimBackslashes(std::move(root));
    if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') return;
    if (std::none_of(roots.begin(), roots.end(), [&](const auto& r) { return !_wcsicmp(r.c_str(), root.c_str()); }))
        roots.push_back(std::move(root));
}

std::wstring parseRegistryPath(std::wstring raw, bool isFileIcon) {
    const auto first = raw.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    raw.erase(0, first);
    while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'\t')) raw.pop_back();
    if (raw.empty()) return {};
    if (raw.front() == L'"') {
        const auto close = raw.find(L'"', 1);
        if (close == std::wstring::npos) return {};
        raw = raw.substr(1, close - 1);
    }
    if (isFileIcon) {
        const auto slash = raw.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        raw.resize(slash);
    }
    return trimBackslashes(std::move(raw));
}

void scanUninstallAntigravity(HKEY hive, const wchar_t* subkey, std::vector<std::wstring>& roots) {
    HKEY parent = nullptr;
    if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &parent) != ERROR_SUCCESS) return;
    wchar_t name[256]{};
    for (DWORD index = 0;; ++index) {
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        const LONG rc = RegEnumKeyExW(parent, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        HKEY entry = nullptr;
        if (RegOpenKeyExW(parent, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) continue;
        wchar_t display[256]{};
        DWORD displayBytes = sizeof(display);
        if (RegGetValueW(entry, nullptr, L"DisplayName", RRF_RT_REG_SZ, nullptr, display, &displayBytes) == ERROR_SUCCESS) {
            const bool matches = _wcsnicmp(display, L"Antigravity", 11) == 0 &&
                                 (display[11] == L'\0' || display[11] == L' ' || display[11] == L'-');
            if (matches) {
                wchar_t value[MAX_PATH * 2]{};
                DWORD bytes = sizeof(value);
                if (RegGetValueW(entry, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS)
                    addUniqueRoot(roots, parseRegistryPath(value, false));
                bytes = sizeof(value);
                if (RegGetValueW(entry, nullptr, L"DisplayIcon", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS)
                    addUniqueRoot(roots, parseRegistryPath(value, true));
            }
        }
        RegCloseKey(entry);
    }
    RegCloseKey(parent);
}

void antigravityRoot(const std::wstring& root, std::vector<std::wstring>& paths) {
    const auto clean = trimBackslashes(root);
    if (!safeDirectory(clean)) return;
    includeFile(paths, clean + L"\\Antigravity.exe");
    includeFile(paths, clean + L"\\Antigravity IDE.exe");
    const auto resources = clean + L"\\resources";
    if (!safeDirectory(resources)) return;
    const auto bin = resources + L"\\bin";
    if (safeDirectory(bin)) {
        for (const auto file : {L"\\language_server.exe",
                                L"\\language_server_windows_x64.exe",
                                L"\\language_server_windows_arm64.exe"})
            includeFile(paths, bin + file);
    }
    const auto appDir = resources + L"\\app";
    const auto extDir = appDir + L"\\extensions";
    const auto agyExt = extDir + L"\\antigravity";
    const auto extBin = agyExt + L"\\bin";
    if (safeDirectory(appDir) && safeDirectory(extDir) && safeDirectory(agyExt) && safeDirectory(extBin)) {
        for (const auto file : {L"\\language_server.exe",
                                L"\\language_server_windows_x64.exe",
                                L"\\language_server_windows_arm64.exe"})
            includeFile(paths, extBin + file);
    }
}

void antigravityPluginBins(const std::wstring& userProfile, std::vector<std::wstring>& paths) {
    const auto profile = trimBackslashes(userProfile);
    if (!safeDirectory(profile)) return;
    const auto localBin = profile + L"\\.local\\bin";
    if (safeDirectory(profile + L"\\.local") && safeDirectory(localBin))
        includeFile(paths, localBin + L"\\agy.exe");
    const auto gemini = profile + L"\\.gemini";
    if (!safeDirectory(gemini)) return;
    for (const auto stateName : {L"\\antigravity", L"\\antigravity-ide", L"\\antigravity-cli"}) {
        const auto stateDir = gemini + stateName;
        const auto pluginData = stateDir + L"\\plugin_data";
        if (!safeDirectory(stateDir) || !safeDirectory(pluginData)) continue;
        WIN32_FIND_DATAW pluginEntry{};
        HANDLE findPlugin = FindFirstFileW((pluginData + L"\\*").c_str(), &pluginEntry);
        if (findPlugin == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring pluginName = pluginEntry.cFileName;
            if (pluginName == L"." || pluginName == L"..") continue;
            if (!(pluginEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (pluginEntry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            const auto binDir = pluginData + L"\\" + pluginName + L"\\bin";
            if (!safeDirectory(binDir)) continue;
            WIN32_FIND_DATAW exeEntry{};
            HANDLE findExe = FindFirstFileW((binDir + L"\\*.exe").c_str(), &exeEntry);
            if (findExe == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(exeEntry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                    includeFile(paths, binDir + L"\\" + exeEntry.cFileName);
            } while (FindNextFileW(findExe, &exeEntry));
            FindClose(findExe);
        } while (FindNextFileW(findPlugin, &pluginEntry));
        FindClose(findPlugin);
    }
}

std::vector<std::wstring> packageRoots(const wchar_t* family) {
    // Resolve at runtime so Windows 7 can still start and use conventional installs.
    HMODULE kernel = GetModuleHandleW(L"kernel32");
    using List = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, PWSTR);
    using Locate = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR);
    auto list = reinterpret_cast<List>(GetProcAddress(kernel, "GetPackagesByPackageFamily"));
    auto locate = reinterpret_cast<Locate>(GetProcAddress(kernel, "GetPackagePathByFullName"));
    UINT32 count = 0, length = 0;
    std::vector<std::wstring> roots;
    if (!list || !locate || list(family, &count, nullptr, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !count) return roots;
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buf(length);
    if (list(family, &count, names.data(), &length, buf.data()) != ERROR_SUCCESS) return roots;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 size = 0;
        if (locate(names[i], &size, nullptr) != ERROR_INSUFFICIENT_BUFFER) continue;
        std::vector<wchar_t> path(size);
        if (locate(names[i], &size, path.data()) == ERROR_SUCCESS) roots.emplace_back(path.data());
    }
    return roots;
}

void codexWorkers(const std::wstring& bin, std::vector<std::wstring>& paths) {
    includeFile(paths, bin + L"codex.exe");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    for (BOOL ok = Process32FirstW(snapshot, &item); ok; ok = Process32NextW(snapshot, &item)) {
        if (_wcsicmp(item.szExeFile, L"codex.exe")) continue;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.th32ProcessID);
        if (!proc) continue;
        wchar_t buf[32768];
        DWORD len = 32768;
        bool found = QueryFullProcessImageNameW(proc, 0, buf, &len) != FALSE;
        CloseHandle(proc);
        if (!found || _wcsnicmp(buf, bin.c_str(), bin.size())) continue;
        std::wstring relative(buf + bin.size());
        if (relative.size() == 26 && relative.find_first_not_of(L"0123456789abcdefABCDEF") == 16 &&
            !_wcsicmp(relative.c_str() + 16, L"\\codex.exe"))
            includeFile(paths, buf);
    }
    CloseHandle(snapshot);
}

void gitFiles(const std::wstring& root, std::vector<std::wstring>& paths) {
    const auto clean = trimBackslashes(root);
    if (!safeDirectory(clean)) return;
    for (auto file : {L"\\cmd\\git.exe", L"\\bin\\git.exe", L"\\mingw64\\bin\\git.exe",
              L"\\mingw64\\libexec\\git-core\\git.exe", L"\\mingw64\\libexec\\git-core\\git-remote-http.exe",
              L"\\mingw64\\libexec\\git-core\\git-remote-https.exe", L"\\mingw64\\bin\\git-lfs.exe",
              L"\\cmd\\git-lfs.exe", L"\\usr\\bin\\ssh.exe", L"\\mingw64\\bin\\git-credential-manager.exe"})
        includeFile(paths, clean + file);
}
}

void collectAntigravityApps(const std::wstring& localAppData, const std::wstring& programFiles,
                            const std::wstring& userProfile, const std::vector<std::wstring>& extraRoots,
                            std::vector<std::wstring>& paths) {
    std::vector<std::wstring> roots;
    if (!localAppData.empty()) {
        const auto local = trimBackslashes(localAppData);
        addUniqueRoot(roots, local + L"\\Programs\\Antigravity");
        addUniqueRoot(roots, local + L"\\Programs\\Antigravity IDE");
        const auto agyRoot = local + L"\\agy";
        const auto agyBin = agyRoot + L"\\bin";
        if (safeDirectory(agyRoot) && safeDirectory(agyBin))
            includeFile(paths, agyBin + L"\\agy.exe");
    }
    if (!programFiles.empty()) {
        const auto pf = trimBackslashes(programFiles);
        addUniqueRoot(roots, pf + L"\\Antigravity");
        addUniqueRoot(roots, pf + L"\\Antigravity IDE");
    }
    for (const auto& extra : extraRoots) addUniqueRoot(roots, extra);
    for (const auto& root : roots) antigravityRoot(root, paths);
    if (!userProfile.empty()) antigravityPluginBins(userProfile, paths);
}

bool resolveApps(unsigned selected, std::vector<std::wstring>& output, std::wstring& error) {
    std::vector<std::wstring> paths;
    std::wstring local, programFiles, profile;
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir))) { local = dir; CoTaskMemFree(dir); dir = nullptr; }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, 0, nullptr, &dir))) { programFiles = dir; CoTaskMemFree(dir); dir = nullptr; }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &dir))) { profile = dir; CoTaskMemFree(dir); dir = nullptr; }
    const wchar_t* names[] = {L"Codex", L"Claude", L"Antigravity", L"Git · gh"};
    for (unsigned group = 0; group < 4; ++group) {
        if (!(selected & (1u << group))) continue;
        const size_t before = paths.size();
        if (group == 0) {
            for (const auto& root : packageRoots(L"OpenAI.Codex_2p2nqsd0c76g0"))
                for (auto name : {L"\\app\\ChatGPT.exe", L"\\app\\Codex.exe", L"\\app\\resources\\codex.exe"})
                    includeFile(paths, root + name);
            if (!local.empty()) codexWorkers(local + L"\\OpenAI\\Codex\\bin\\", paths);
        } else if (group == 1) {
            for (const auto& root : packageRoots(L"Claude_pzs8sxrjxfjjc"))
                includeFile(paths, root + L"\\app\\claude.exe");
        } else if (group == 2) {
            std::vector<std::wstring> registryRoots;
            constexpr auto uninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
            scanUninstallAntigravity(HKEY_CURRENT_USER, uninstall, registryRoots);
            scanUninstallAntigravity(HKEY_LOCAL_MACHINE, uninstall, registryRoots);
            scanUninstallAntigravity(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", registryRoots);
            collectAntigravityApps(local, programFiles, profile, registryRoots, paths);
        } else if (group == 3) {
            wchar_t git[MAX_PATH]{};
            DWORD len = sizeof(git);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GitForWindows", L"InstallPath", RRF_RT_REG_SZ, nullptr, git, &len) == ERROR_SUCCESS)
                gitFiles(git, paths);
            if (!programFiles.empty()) {
                gitFiles(programFiles + L"\\Git", paths);
                includeFile(paths, programFiles + L"\\GitHub CLI\\gh.exe");
            }
            if (!local.empty()) {
                const auto desktop = local + L"\\GitHubDesktop\\";
                WIN32_FIND_DATAW entry{};
                HANDLE found = FindFirstFileW((desktop + L"app-*").c_str(), &entry);
                if (found != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                        std::wstring root = desktop + entry.cFileName;
                        includeFile(paths, root + L"\\GitHubDesktop.exe");
                        gitFiles(root + L"\\resources\\app\\git", paths);
                    } while (FindNextFileW(found, &entry));
                    FindClose(found);
                }
            }
        }
        if (before == paths.size()) {
            error = std::wstring(names[group]) + L" 실행 파일을 찾지 못했습니다. 설치 계정에서 앱을 열거나 이 예외를 해제하세요.";
            return false;
        }
    }
    output = std::move(paths);
    error.clear();
    return true;
}
}
