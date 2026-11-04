#include "app.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <ws2tcpip.h>

void testHistory();
void testEngine();
void testWfpRollback();
void testWfpProbe();
void testCiAppException();
namespace tec { void testCiGithubProxy(); }

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
    std::vector<std::wstring> expanded;
    check(tec::expandNetworks({L"172.16.1.*",L"172.17.*.*",L"172.18.1.2?"},expanded,error) &&
          expanded==std::vector<std::wstring>{L"172.16.1.0/24",L"172.17.0.0/16",L"172.18.1.20/30",L"172.18.1.24/30",L"172.18.1.28/31"},"exact wildcard ranges");
    for (auto bad : {L"*.*.*.*",L"172.1.*",L"172.3?.999.*",L"10.*.*.1",L"a.b.*.1",L"10.0.?.*/24"})
        check(!tec::expandNetworks({bad},expanded,error),"overbroad, excessive or malformed wildcard");
    for(auto host : {"github.com","api.github.com","github.githubassets.com","release-assets.githubusercontent.com"})
        check(tec::githubHost(host),"GitHub host");
    for(auto host : {"notgithub.com","github.com.evil.test","127.0.0.1","user@github.com","xx..github.com","gitlab.com"})
        check(!tec::githubHost(host),"off-site relay refused");
    auto invalid = config; invalid.networks = {L"::/0"};
    check(!tec::validate(invalid,error), "default allow-all route");
    invalid = config; invalid.dns = {L"192.0.2.0/24"};
    check(!tec::validate(invalid,error), "DNS must be a single endpoint");
    wchar_t tmp[MAX_PATH]{}, path[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH,tmp) && GetTempFileNameW(tmp,L"tec",0,path), "temporary path");
    check(DeleteFileW(path) && CreateDirectoryW(path,nullptr), "temporary directory");
    const std::wstring dir(path);
    check(!tec::trustedInstallPath(dir,false,error), "user-controlled directory cannot host an elevated app");
    for (const auto invalidPath : {L"D:\\",L"relative\\app.exe",L"\\\\server\\share\\app.exe",L"D:\\Apps\\..\\app.exe",L"D:\\Apps\\trailing.\\app.exe"})
        check(!tec::trustedInstallPath(invalidPath,true,error), "unsafe installation path rejected");
    check(tec::saveSettings(dir,config,error), "write configuration");
    tec::Settings loaded;
    check(tec::loadSettings(dir,loaded,error) && !loaded.apps,"legacy settings default exceptions off");
    config.apps=25; config.networks={L"172.16.*.*"};
    check(tec::saveSettings(dir,config,error),"write opt-in exceptions");
    check(!tec::saveSettings(dir,invalid,error) && tec::loadSettings(dir,loaded,error) &&
          loaded.networks == config.networks && loaded.dns == config.dns && loaded.guid == config.guid && loaded.apps==25,
          "invalid replacement preserves last confirmed settings");
    DeleteFileW((dir+L"\\settings.dat").c_str()); RemoveDirectoryW(path);
}

int main(int argc, char** argv) {
    try {
        if (argc==2 && std::strcmp(argv[1],"--ci-app-exception")==0) {
            testCiAppException(); tec::testCiGithubProxy();
            std::puts("PASS: selected app only, direct recovery, GitHub HTTPS via relay and unrelated host refused."); return 0;
        }
        if (argc==2 && std::strcmp(argv[1],"--inspect-apps")==0) {
            for(unsigned i=0;i<4;++i) {
                std::vector<std::wstring> paths; std::wstring error;
                if(!tec::resolveApps(1u<<i,paths,error)) {std::puts("not installed");continue;}
                for(const auto& p:paths) std::wprintf(L"%ls\n",p.c_str());
            }
            return 0;
        }
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
        if (argc == 2 && std::strcmp(argv[1], "--ci-install-path") == 0) {
            // Installer smoke tests only: no engine or network policy is opened.
            std::wstring error;
            const auto exe=tec::installedExe();
            check(!exe.empty() && tec::trustedInstallPath(exe,true,error), "registered executable is protected");
            const auto dir=tec::storageDir(error);
            check(dir==exe.substr(0,exe.find_last_of(L'\\'))+L"\\Data", "storage follows registered installation");
            tec::Settings config{L"{FE379A83-E678-405B-8581-6E2E7CAAA280}",{L"192.0.2.0/24"},{}};
            check(tec::saveSettings(dir,config,error), "settings in selected directory");
            tec::Settings loaded;
            check(tec::loadSettings(dir,loaded,error) && loaded.guid==config.guid, "reload selected-directory settings");
            std::puts("PASS: protected registered executable and installation-local settings.");
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
