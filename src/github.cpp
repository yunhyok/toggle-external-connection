#include "app.h"
#include <ws2tcpip.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#ifdef TEC_TESTING
#include <stdexcept>
#endif

namespace tec {
bool githubHost(const std::string& host) {
    if (host.empty() || host.size()>253 || host.front()=='.' || host.back()=='.') return false;
    size_t begin=0;
    while (begin<host.size()) {
        const size_t end=host.find('.',begin);
        const auto label=host.substr(begin,end-begin);
        if (label.empty() || label.size()>63 || label.front()=='-' || label.back()=='-' ||
            label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=label.npos) return false;
        if (end==host.npos) break;
        begin=end+1;
    }
    for (std::string zone : {"github.com","githubassets.com","githubusercontent.com"}) {
        if (host==zone || (host.size()>zone.size() && host.compare(host.size()-zone.size()-1,zone.size()+1,"."+zone)==0)) return true;
    }
    return false;
}
namespace {
struct Socket {
    SOCKET s;
    explicit Socket(SOCKET value):s(value){}
    ~Socket(){if(s!=INVALID_SOCKET) closesocket(s);}
    Socket(const Socket&)=delete;
};
bool sendAll(SOCKET s,const char* ptr,int len) {
    while (len>0) {
        int sent=send(s,ptr,len,0);
        if (sent<=0) return false;
        ptr+=sent; len-=sent;
    }
    return true;
}
void timeouts(SOCKET s) {
    DWORD timeout=5000;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
}
void tunnel(SOCKET client,const std::atomic<bool>& stopping) {
    Socket local(client); timeouts(client);
    char buf[16384]; std::string header;
    auto end=header.npos;
    while (end==header.npos && header.size()<8192 && !stopping) {
        int n=recv(client,buf,2048,0); if(n<=0) return;
        header.append(buf,n); end=header.find("\r\n\r\n");
    }
    const auto space=header.find(' ',8);
    if (end==header.npos || space==header.npos || header.compare(0,8,"CONNECT ")!=0) return;
    auto authority=header.substr(8,space-8);
    for(auto& c:authority) if(c>='A' && c<='Z') c+=32;
    if(authority.size()<=4 || authority.compare(authority.size()-4,4,":443")!=0 ||
       !githubHost(authority.substr(0,authority.size()-4))) {
        static const char denied[]="HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        sendAll(client,denied,sizeof(denied)-1); return;
    }
    std::string host=authority.substr(0,authority.size()-4);
    addrinfo hints{},*addresses=nullptr; hints.ai_socktype=SOCK_STREAM; hints.ai_protocol=IPPROTO_TCP;
    if (getaddrinfo(host.c_str(),"443",&hints,&addresses)!=0) return;
    SOCKET upstream=INVALID_SOCKET;
    for(auto a=addresses;a && !stopping;a=a->ai_next) {
        SOCKET s=socket(a->ai_family,SOCK_STREAM,IPPROTO_TCP); if(s==INVALID_SOCKET) continue;
        u_long nonblocking=1;
        if(ioctlsocket(s,FIONBIO,&nonblocking)==0) {
            int rc=connect(s,a->ai_addr,static_cast<int>(a->ai_addrlen));
            if(rc==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) {
                fd_set writes,errors; FD_ZERO(&writes); FD_SET(s,&writes); FD_ZERO(&errors); FD_SET(s,&errors);
                timeval tv{4,0}; int err=0,size=sizeof(err);
                rc=(select(0,nullptr,&writes,&errors,&tv)>0 && FD_ISSET(s,&writes) &&
                    getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&err),&size)==0 && !err)?0:-1;
            }
            nonblocking=0;
            if(rc==0 && ioctlsocket(s,FIONBIO,&nonblocking)==0) {upstream=s;break;}
        }
        closesocket(s);
    }
    freeaddrinfo(addresses); Socket remote(upstream);
    if(upstream==INVALID_SOCKET) return;
    timeouts(upstream);
    static const char ready[]="HTTP/1.1 200 Connection Established\r\n\r\n";
    if (!sendAll(client,ready,sizeof(ready)-1) || !sendAll(upstream,header.data()+end+4,static_cast<int>(header.size()-end-4))) return;
    while(!stopping) {
        fd_set fds; FD_ZERO(&fds); FD_SET(client,&fds); FD_SET(upstream,&fds); timeval tv{1,0};
        int n=select(0,&fds,nullptr,nullptr,&tv); if(n<0) return;
        for(auto from : {client,upstream}) if(FD_ISSET(from,&fds)) {
            int received=recv(from,buf,sizeof(buf),0);
            if(received<=0 || !sendAll(from==client ? upstream : client,buf,received)) return;
        }
    }
}
std::wstring browserExe() {
    // Use the installed browser under Program Files. The elevated tray never starts a browser directly.
    for(auto folder : {FOLDERID_ProgramFilesX64,FOLDERID_ProgramFilesX86}) {
        PWSTR path=nullptr;
        if(FAILED(SHGetKnownFolderPath(folder,0,nullptr,&path))) continue;
        std::wstring root(path); CoTaskMemFree(path);
        for(auto rel : {L"\\Microsoft\\Edge\\Application\\msedge.exe",L"\\Google\\Chrome\\Application\\chrome.exe"}) {
            auto exe=root+rel;
            if(GetFileAttributesW(exe.c_str())!=INVALID_FILE_ATTRIBUTES) return exe;
        }
    }
    return {};
}
int notify(const wchar_t* text) {MessageBoxW(nullptr,text,L"GitHub 웹",MB_OK|MB_ICONINFORMATION);return 1;}
}
int githubWindow() {
    if(IsUserAnAdmin()) return notify(L"GitHub 웹은 일반 사용자로 시작 메뉴에서 여세요.");
    auto browser=browserExe();
    if(browser.empty()) return notify(L"GitHub 전용 창을 열려면 Microsoft Edge 또는 Google Chrome 데스크톱 브라우저가 필요합니다.");
    HANDLE one=CreateMutexW(nullptr,FALSE,L"Local\\TEC.GitHubBrowser");
    if(!one || GetLastError()==ERROR_ALREADY_EXISTS) {if(one) CloseHandle(one);return notify(L"이미 열린 GitHub 전용 창을 사용하세요.");}
    struct Mutex {HANDLE h;~Mutex(){CloseHandle(h);}} mutex{one};
    WSADATA data{}; if(WSAStartup(MAKEWORD(2,2),&data)!=0) return 1;
    struct Winsock {~Winsock(){WSACleanup();}} winsock;
    Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    BOOL exclusive=TRUE; int len=sizeof(address);
    if(listener.s==INVALID_SOCKET || setsockopt(listener.s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))!=0 ||
       bind(listener.s,reinterpret_cast<sockaddr*>(&address),len)!=0 || listen(listener.s,SOMAXCONN)!=0 ||
       getsockname(listener.s,reinterpret_cast<sockaddr*>(&address),&len)!=0) return notify(L"로컬 GitHub 연결을 열 수 없습니다.");
    PWSTR local=nullptr; if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))) return 1;
    std::wstring profile=std::wstring(local)+L"\\Toggle External Connection\\GitHubBrowser"; CoTaskMemFree(local);
    std::wstring cmd=L"\""+browser+L"\" --user-data-dir=\""+profile+
        L"\" --no-first-run --no-default-browser-check --disable-background-mode --disable-quic --proxy-bypass-list=\"<-loopback>\" --proxy-server=http://127.0.0.1:"+
        std::to_wstring(ntohs(address.sin_port))+L" --new-window https://github.com/";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION proc{};
    if(!CreateProcessW(browser.c_str(),&cmd[0],nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&proc)) return notify(L"브라우저를 시작할 수 없습니다.");
    CloseHandle(proc.hThread);
    std::atomic<bool> stopping{false};
    std::vector<std::future<void>> workers;
    workers.reserve(64);
    while(WaitForSingleObject(proc.hProcess,0)==WAIT_TIMEOUT) {
        workers.erase(std::remove_if(workers.begin(),workers.end(),[](auto& w){return w.wait_for(std::chrono::seconds(0))==std::future_status::ready;}),workers.end());
        fd_set incoming; FD_ZERO(&incoming); FD_SET(listener.s,&incoming); timeval tv{0,200000};
        if(select(0,&incoming,nullptr,nullptr,&tv)<=0) continue;
        SOCKET client=accept(listener.s,nullptr,nullptr); if(client==INVALID_SOCKET) continue;
        if(workers.size()>=64) {closesocket(client);continue;}
        try {workers.push_back(std::async(std::launch::async,tunnel,client,std::cref(stopping)));}
        catch(...) {closesocket(client);}
    }
    stopping=true; CloseHandle(proc.hProcess);
    return 0;
}

#ifdef TEC_TESTING
void testCiGithubProxy() {
    wchar_t ci[16]{},system[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"GITHUB_ACTIONS",ci,16) || wcscmp(ci,L"true") || !GetSystemDirectoryW(system,MAX_PATH))
        throw std::runtime_error("CI only");
    WSADATA ws{}; if(WSAStartup(MAKEWORD(2,2),&ws)) throw std::runtime_error("WSAStartup");
    struct Cleanup {~Cleanup(){WSACleanup();}} cleanup;
    Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);int len=sizeof(addr);
    if(bind(listener.s,reinterpret_cast<sockaddr*>(&addr),len) || listen(listener.s,4) ||
       getsockname(listener.s,reinterpret_cast<sockaddr*>(&addr),&len)) throw std::runtime_error("proxy test listener");
    for(bool allow : {true,false}) {
        std::atomic<bool> stopping{false};
        auto serve=std::async(std::launch::async,[&] {
            fd_set f;FD_ZERO(&f);FD_SET(listener.s,&f);timeval tv{12,0};
            if(select(0,&f,nullptr,nullptr,&tv)<=0) throw std::runtime_error("curl did not reach proxy");
            tunnel(accept(listener.s,nullptr,nullptr),stopping);
        });
        std::wstring curl=std::wstring(system)+L"\\curl.exe";
        std::wstring command=L"\""+curl+L"\" --disable --noproxy \"\" --silent --fail --max-time 15 --output NUL --proxy http://127.0.0.1:"+
            std::to_wstring(ntohs(addr.sin_port))+(allow?L" https://api.github.com/repos/git/git":L" https://example.com/");
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION proc{};
        if(!CreateProcessW(curl.c_str(),&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&proc))
            throw std::runtime_error("curl unavailable");
        WaitForSingleObject(proc.hProcess,20000);DWORD code=0;GetExitCodeProcess(proc.hProcess,&code);
        CloseHandle(proc.hThread);CloseHandle(proc.hProcess);stopping=true;serve.get();
        if ((code==0)!=allow) throw std::runtime_error("GitHub web HTTPS relay mismatch, curl="+std::to_string(code));
    }
}
#endif
}
