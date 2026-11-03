#include "app.h"
#include <cstdio>

int wmain(int argc,wchar_t** argv) {
    std::wstring error;
    bool ok=false;
    if (argc==3 && wcscmp(argv[1],L"--prepare")==0)
        ok=tec::prepareInstallDirectory(argv[2],error) && tec::protectInstalledFiles(argv[2],error);
    else if (argc==3 && wcscmp(argv[1],L"--seal")==0)
        ok=tec::protectInstalledFiles(argv[2],error);
    else if (argc==3 && wcscmp(argv[1],L"--check")==0)
        ok=tec::trustedInstallPath(argv[2],true,error);
    else if (argc==3 && wcscmp(argv[1],L"--migrate")==0)
        ok=tec::migrateInstallData(argv[2],error);
    else return 2;
    if (!ok) std::fwprintf(stderr,L"%ls\n",error.c_str());
    return ok ? 0 : 1;
}
