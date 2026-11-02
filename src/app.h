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
};
struct Adapter {
    std::wstring guid, name;
    Policy suggested;
};
struct Settings {
    std::wstring guid;
    std::vector<std::wstring> networks, dns;
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
std::vector<Adapter> adapters(std::wstring& error);
bool validate(const Settings& settings, std::wstring& error);
// Empty on failure. Production only. Tests and previews never use this directory.
std::wstring storageDir(std::wstring& error);
std::wstring installedExe();
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
