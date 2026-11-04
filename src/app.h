#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tec {
struct Policy {
    unsigned long long luid = 0;
    std::vector<std::wstring> networks, dns;
    std::vector<std::wstring> dhcp;
    std::vector<std::wstring> appPaths;
    std::wstring webHelper;
};
struct Adapter {
    std::wstring guid, name;
    Policy suggested;
};
struct Settings {
    std::wstring guid;
    std::vector<std::wstring> networks, dns;
    unsigned apps = 0;
};
struct Attempt {
    FILETIME time{};
    std::wstring path, ip;
    int port = -1, protocol = -1;
};
struct Row {
    Attempt first;
    FILETIME last{};
    unsigned long long count = 0;
    std::wstring hint;
};

bool parsePrefix(const std::wstring& text, int& family, std::array<unsigned char,16>& bytes, unsigned& bits);
bool expandNetworks(const std::vector<std::wstring>& input, std::vector<std::wstring>& output, std::wstring& error);
bool resolveApps(unsigned selected, std::vector<std::wstring>& paths, std::wstring& error);
int githubWindow();
bool githubHost(const std::string& host);
bool googleLoginHost(const std::string& host);
bool googleLoginAllowed();
DWORD setGoogleLoginAllowed(bool enabled);
std::vector<Adapter> adapters(std::wstring& error);
bool validate(const Settings& settings, std::wstring& error);
// Empty on failure. Production only. Tests and previews never use this directory.
std::wstring storageDir(std::wstring& error);
std::wstring installedExe();
// Reject paths that an unelevated account can replace before elevation.
bool trustedInstallPath(const std::wstring& path, bool executable, std::wstring& error);
bool prepareInstallDirectory(const std::wstring& path, std::wstring& error);
bool protectInstalledFiles(const std::wstring& path, std::wstring& error);
bool migrateInstallData(const std::wstring& path, std::wstring& error);
bool loadSettings(const std::wstring& dir, Settings& settings, std::wstring& error);
bool saveSettings(const std::wstring& dir, const Settings& settings, std::wstring& error);

class Engine {
public:
    Engine();
    ~Engine();
    bool start(const Policy&, std::function<void(Attempt)>, std::wstring& error, std::wstring& logWarning);
    void stop();
    bool active() const;
    bool healthy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
class History {
public:
    explicit History(const std::wstring& dir);
    ~History();
    void push(Attempt);
    void flush();
    void maintain();
    std::vector<Row> rows();
    std::wstring error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
}
