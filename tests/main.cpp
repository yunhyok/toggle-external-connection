#include "app.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <ws2tcpip.h>

void testHistory();
void testEngine();
void testWfpRollback();
void testWfpProbe();

static void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

static void testSettings() {
    int family = 0; unsigned bits = 0; std::array<unsigned char,16> bytes{};
    check(tec::parsePrefix(L"192.0.2.128/25",family,bytes,bits) && family == AF_INET &&
          bits == 25 && bytes[3] == 128, "IPv4 subnet");
    check(tec::parsePrefix(L"2001:db8:abcd::/48",family,bytes,bits) && family == AF_INET6 &&
          bytes[4] == 0xab && bits == 48, "IPv6 subnet");
    check(tec::parsePrefix(L"2001:db8::4",family,bytes,bits) && bits == 128, "IPv6 host");
    for (auto s : {L"192.0.2.3/24", L"999.0.0.1", L"2001:db8::/129", L"1.2.3.4/1x",
                  L"192.0.2.1/32/1", L"hostname", L""})
        check(!tec::parsePrefix(s,family,bytes,bits), "malformed or host-bit prefix");
    tec::Settings config{L"{FE379A83-E678-405B-8581-6E2E7CAAA280}",{L"192.0.2.0/24"},{L"2001:db8::53"}};
    std::wstring error;
    check(tec::validate(config,error), "valid settings");
    auto invalid = config; invalid.networks = {L"::/0"};
    check(!tec::validate(invalid,error), "default allow-all route");
    invalid = config; invalid.dns = {L"192.0.2.0/24"};
    check(!tec::validate(invalid,error), "DNS must be a single endpoint");
    wchar_t tmp[MAX_PATH]{}, path[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH,tmp) && GetTempFileNameW(tmp,L"tec",0,path), "temporary path");
    check(DeleteFileW(path) && CreateDirectoryW(path,nullptr), "temporary directory");
    const std::wstring dir(path);
    check(tec::saveSettings(dir,config,error), "write configuration");
    tec::Settings loaded;
    check(!tec::saveSettings(dir,invalid,error) && tec::loadSettings(dir,loaded,error) &&
          loaded.networks == config.networks && loaded.dns == config.dns && loaded.guid == config.guid,
          "invalid replacement preserves last confirmed settings");
    DeleteFileW((dir+L"\\settings.dat").c_str()); RemoveDirectoryW(path);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--ci-wfp-rollback") == 0) {
            testWfpRollback();
            std::puts("PASS: Windows accepted staged WFP rules; transaction aborted.");
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--ci-wfp-probe") == 0) {
            testWfpProbe();
            std::puts("PASS: test process TCP and UDP denial events matched the destination and application.");
            return 0;
        }
        if (argc != 1) return 2;
        testSettings(); testEngine(); testHistory();
        std::puts("PASS: settings, filter/attribution policy, history and retention (offline).");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,"FAIL: %s\n",ex.what()); return 1;
    }
}
