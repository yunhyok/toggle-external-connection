#include "app.h"
#include <aclapi.h>
#include <sddl.h>
#include <cstring>

namespace tec {
namespace {
bool fail(std::wstring& error, const std::wstring& path) {
    error = L"Unsafe installation path: " + path + L" (Windows " + std::to_wstring(GetLastError()) + L")";
    return false;
}

bool trustedSid(PSID sid) {
    if (!sid) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID installer = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer)) return false;
    const bool trusted = EqualSid(sid, installer) != FALSE;
    LocalFree(installer);
    return trusted;
}

bool validPath(const std::wstring& path, std::wstring& error) {
    if (path.size() < 4 || path.size() > MAX_PATH - 40 || path[1] != L':' || path[2] != L'\\' ||
        path.find_first_of(L"/\"<>|*?") != path.npos || path.find(L':', 2) != path.npos || path.back() == L'\\') return fail(error,path);
    for (size_t begin=3; begin<path.size();) {
        const auto end=path.find(L'\\',begin);
        const auto component=path.substr(begin,end==path.npos ? path.size()-begin : end-begin);
        if (component.empty() || component.back()==L'.' || component.back()==L' ' || component.find(L'\0')!=component.npos) return fail(error,path);
        begin=end==path.npos ? path.size() : end+1;
    }
    const auto root=path.substr(0,3);
    DWORD flags=0;
    if (GetDriveTypeW(root.c_str())!=DRIVE_FIXED ||
        !GetVolumeInformationW(root.c_str(),nullptr,0,nullptr,nullptr,&flags,nullptr,0) || !(flags&FILE_PERSISTENT_ACLS)) return fail(error,path);
    return true;
}

// Check the object itself; inherited-only ACEs do not grant access to it.
bool trustedObject(const std::wstring& path, bool directory, bool parent, std::wstring& error) {
    HANDLE file=CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if (file==INVALID_HANDLE_VALUE) return fail(error,path);
    BY_HANDLE_FILE_INFORMATION info{};
    PSID owner=nullptr; PACL acl=nullptr; PSECURITY_DESCRIPTOR descriptor=nullptr;
    const bool opened=GetFileInformationByHandle(file,&info) &&
        !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory;
    const DWORD result=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        &owner,nullptr,&acl,nullptr,&descriptor);
    CloseHandle(file);
    bool safe=opened && result==ERROR_SUCCESS && trustedSid(owner) && acl;
    const DWORD mutation=WRITE_DAC|WRITE_OWNER|DELETE|FILE_DELETE_CHILD|GENERIC_ALL|GENERIC_WRITE|
        (parent ? 0 : FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES);
    for (DWORD i=0; safe && i<acl->AceCount; ++i) {
        void* entry=nullptr;
        if (!GetAce(acl,i,&entry)) { safe=false; break; }
        const auto header=static_cast<ACE_HEADER*>(entry);
        if (header->AceFlags&INHERIT_ONLY_ACE || header->AceType==ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType!=ACCESS_ALLOWED_ACE_TYPE) { safe=false; break; }
        const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if ((ace->Mask&mutation) && !trustedSid(&ace->SidStart)) safe=false;
    }
    if (descriptor) LocalFree(descriptor);
    return safe || fail(error,path);
}

bool protect(const std::wstring& path, bool directory, std::wstring& error, bool privateData=false) {
    HANDLE file=CreateFileW(path.c_str(),WRITE_DAC|WRITE_OWNER|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if (file==INVALID_HANDLE_VALUE) return fail(error,path);
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory && (directory || info.nNumberOfLinks==1);
    PSECURITY_DESCRIPTOR sd=nullptr;
    if (ok) ok=ConvertStringSecurityDescriptorToSecurityDescriptorW(privateData ?
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" :
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr)!=FALSE;
    PSID owner=nullptr; PACL acl=nullptr; BOOL present=FALSE,defaulted=FALSE;
    if (ok) ok=GetSecurityDescriptorOwner(sd,&owner,&defaulted) && GetSecurityDescriptorDacl(sd,&present,&acl,&defaulted);
    if (ok) {
        const DWORD code=SetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|
            PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,acl,nullptr);
        ok=code==ERROR_SUCCESS;
        if (!ok) SetLastError(code);
    }
    if (sd) LocalFree(sd);
    CloseHandle(file);
    return ok || fail(error,path);
}

bool availableTarget(const std::wstring& path, std::wstring& error) {
    WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND || fail(error,path);
    bool ok=true,programFiles=false;
    do {
        const std::wstring name=data.cFileName;
        if (name==L"." || name==L"..") continue;
        if (name==L"Data") {
            if (!trustedObject(path+L"\\Data",true,false,error)) { ok=false; break; }
            continue;
        }
        if (name!=L"ToggleExternalConnection.exe" && name!=L"TESTING.md" && name!=L"check-lab.ps1" &&
            name!=L"unins000.exe" && name!=L"unins000.dat" && name!=L"unins000.msg") { ok=false; break; }
        programFiles=true;
    } while (FindNextFileW(find,&data));
    const DWORD code=GetLastError();
    FindClose(find);
    if (!ok || code!=ERROR_NO_MORE_FILES) return fail(error,path);
    if (!programFiles) return true;
    const auto exe=path+L"\\ToggleExternalConnection.exe";
    const DWORD size=GetFileVersionInfoSizeW(exe.c_str(),nullptr);
    std::vector<unsigned char> version(size);
    void* product=nullptr; UINT length=0;
    return (size && GetFileVersionInfoW(exe.c_str(),0,size,version.data()) &&
        VerQueryValueW(version.data(),L"\\StringFileInfo\\040904b0\\ProductName",&product,&length) &&
        length && wcscmp(static_cast<const wchar_t*>(product),L"Toggle External Connection")==0) || fail(error,path);
}
}

bool trustedInstallPath(const std::wstring& path, bool executable, std::wstring& error) {
    error.clear();
    if (!validPath(path,error) || !trustedObject(path.substr(0,3),true,true,error)) return false;
    for (size_t end=path.find(L'\\',3); end!=path.npos; end=path.find(L'\\',end+1))
        if (!trustedObject(path.substr(0,end),true,!executable || end!=path.find_last_of(L'\\'),error)) return false;
    return trustedObject(path,!executable,false,error);
}

bool prepareInstallDirectory(const std::wstring& path, std::wstring& error) {
    error.clear();
    if (!validPath(path,error) || !trustedObject(path.substr(0,3),true,true,error)) return false;
    PSECURITY_DESCRIPTOR sd=nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr)) return fail(error,path);
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    bool ok=true;
    for (size_t end=path.find(L'\\',3); ok;) {
        const auto part=end==path.npos ? path : path.substr(0,end);
        const bool exists=GetFileAttributesW(part.c_str())!=INVALID_FILE_ATTRIBUTES;
        if (!exists && !CreateDirectoryW(part.c_str(),&sa)) { ok=fail(error,part); break; }
        // Only the registered installation may have its existing directory hardened.
        const auto registered=installedExe();
        const auto previous=registered.substr(0,registered.find_last_of(L'\\'));
        const bool upgrade=end==path.npos && !registered.empty() && _wcsicmp(previous.c_str(),path.c_str())==0;
        ok=trustedObject(part,true,end!=path.npos,error);
        if (!ok && upgrade) ok=protect(part,true,error);
        if (ok && exists && end==path.npos && !upgrade) ok=availableTarget(part,error);
        if (end==path.npos) break;
        end=path.find(L'\\',end+1);
    }
    LocalFree(sd);
    return ok && trustedInstallPath(path,false,error);
}

std::wstring installedExe() {
    wchar_t path[MAX_PATH]{}; DWORD size=sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{CA78C24A-7964-4F3A-8F40-DA64E9D82A11}_is1",
        L"InstallLocation",RRF_RT_REG_SZ,nullptr,path,&size)!=ERROR_SUCCESS) return {};
    std::wstring result(path);
    while (!result.empty() && result.back()==L'\\') result.pop_back();
    return result.empty() ? result : result+L"\\ToggleExternalConnection.exe";
}

bool protectInstalledFiles(const std::wstring& path, std::wstring& error) {
    if (!trustedInstallPath(path,false,error)) return false;
    for (const auto name : {L"ToggleExternalConnection.exe",L"TESTING.md",L"check-lab.ps1",L"unins000.exe"}) {
        const auto file=path+L"\\"+name;
        if (GetFileAttributesW(file.c_str())!=INVALID_FILE_ATTRIBUTES && !protect(file,false,error)) return false;
    }
    return true;
}

bool migrateInstallData(const std::wstring& path, std::wstring& error) {
    const auto registered=installedExe();
    if (registered.empty()) return fail(error,path);
    const auto old=registered.substr(0,registered.find_last_of(L'\\'));
    const auto source=old+L"\\Data", destination=path+L"\\Data";
    if (_wcsnicmp((old+L"\\").c_str(),(path+L"\\").c_str(),old.size()+1)==0 ||
        _wcsnicmp((path+L"\\").c_str(),(old+L"\\").c_str(),path.size()+1)==0) return fail(error,path);
    if (!prepareInstallDirectory(old,error) || !protectInstalledFiles(old,error) ||
        !trustedInstallPath(path,false,error)) return false;
    if (GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES)
        return GetLastError()==ERROR_FILE_NOT_FOUND || fail(error,source);
    if (!protect(source,true,error,true)) return false;
    PSECURITY_DESCRIPTOR sd=nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&sd,nullptr)) return fail(error,destination);
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const bool created=CreateDirectoryW(destination.c_str(),&sa)!=FALSE;
    const DWORD code=GetLastError();
    LocalFree(sd);
    if ((!created && code!=ERROR_ALREADY_EXISTS) || !protect(destination,true,error,true)) return fail(error,destination);
    WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW((source+L"\\*").c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND || fail(error,source);
    bool ok=true;
    do {
        const std::wstring name=data.cFileName;
        if (name==L"." || name==L"..") continue;
        if (data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) { ok=fail(error,source+L"\\"+name); break; }
        const auto from=source+L"\\"+name, to=destination+L"\\"+name;
        HANDLE input=CreateFileW(from.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        ok=input!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(input,&info) && info.nNumberOfLinks==1 &&
            !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
        if (ok && !CopyFileW(from.c_str(),to.c_str(),TRUE) && GetLastError()!=ERROR_FILE_EXISTS) ok=false;
        if (ok) ok=protect(to,false,error,true);
        HANDLE output=ok ? CreateFileW(to.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr) : INVALID_HANDLE_VALUE;
        ok=ok && output!=INVALID_HANDLE_VALUE;
        while (ok) {
            unsigned char left[16384]{},right[16384]{}; DWORD a=0,b=0;
            ok=ReadFile(input,left,sizeof(left),&a,nullptr) && ReadFile(output,right,sizeof(right),&b,nullptr) && a==b &&
                std::memcmp(left,right,a)==0;
            if (!a) break;
        }
        if (ok) ok=FlushFileBuffers(output)!=FALSE;
        if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
        if (!ok) { fail(error,to); break; }
    } while (FindNextFileW(find,&data));
    const DWORD enumerationError=GetLastError();
    FindClose(find);
    return (ok && enumerationError==ERROR_NO_MORE_FILES) || fail(error,source);
}
}
