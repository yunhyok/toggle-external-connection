#include "app.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cassert>
#include <cwctype>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>

namespace {
constexpr COLORREF background = RGB(32,34,38), panel = RGB(43,46,52);
constexpr COLORREF input = RGB(29,32,37), editor = RGB(21,24,29);
constexpr COLORREF foreground = RGB(236,238,241), muted = RGB(176,183,194);
constexpr COLORREF accent = RGB(56,108,176), focus = RGB(143,193,255);
constexpr UINT trayMessage = WM_APP + 1, trialEnded = WM_APP + 2;
constexpr UINT timerRefresh = 1, timerClick = 2, timerTrial = 3;
enum { navSettings = 1001, navHistory, toggleButton, adapterCombo, networkEdit,
       dnsEdit, saveButton, trialButton, appFilter, ipFilter, historyList,
       selectedPath, refreshButton, firstDev, lastDev=firstDev+4, googleOption, webCommand, exitCommand = 1100, trialCommand };
const wchar_t* devNames[]={L"Codex",L"Claude",L"Antigravity",L"Git · gh",L"GitHub 웹"};
HINSTANCE instance;
UINT taskbarCreated;
HFONT normalFont, titleFont, headingFont;
HBRUSH backgroundBrush, panelBrush, inputBrush, editorBrush;

std::wstring text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    value.resize(GetWindowTextW(control, &value[0], length + 1));
    return value;
}

std::wstring join(const std::vector<std::wstring>& lines) {
    std::wstring value;
    for (const auto& line : lines) { if (!value.empty()) value += L"\r\n"; value += line; }
    return value;
}

std::vector<std::wstring> split(const std::wstring& value) {
    std::vector<std::wstring> lines;
    std::wistringstream stream(value);
    std::wstring line;
    while (std::getline(stream, line)) {
        const auto begin = line.find_first_not_of(L" \t\r");
        if (begin == std::wstring::npos) continue;
        const auto end = line.find_last_not_of(L" \t\r");
        lines.push_back(line.substr(begin, end - begin + 1));
    }
    return lines;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

std::wstring leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring stamp(FILETIME time) {
    FILETIME local{}; SYSTEMTIME date{};
    if (!FileTimeToLocalFileTime(&time, &local) || !FileTimeToSystemTime(&local, &date)) return L"—";
    wchar_t value[40];
    swprintf_s(value, L"%04u-%02u-%02u %02u:%02u:%02u", date.wYear, date.wMonth, date.wDay,
               date.wHour, date.wMinute, date.wSecond);
    return value;
}

struct Ui {
    HWND window{}, settingsPane{}, historyPane{}, adaptersControl{}, networks{}, dns{}, metadata{},
         save{}, trial{}, toggle{}, settingsNav{}, historyNav{}, logControl{}, list{}, appSearch{}, ipSearch{}, path{};
    std::vector<HWND> settingsChildren;
    HWND dev[5]{}, googleLogin{};
    std::vector<tec::Adapter> adapters;
    std::vector<tec::Row> visibleRows;
    tec::Settings saved;
    tec::Engine engine;
    std::future<std::pair<unsigned,bool>> healthCheck;
    std::thread trialDeadline;
    HANDLE trialCancel = nullptr;
    unsigned generation = 0;
    std::unique_ptr<tec::History> history;
    std::wstring dir, notice = L"설정을 확인하고 저장한 뒤 차단을 시작하세요.";
    std::wstring captureIssue, historyIssue;
    bool preview = false, historyMode = false, confirmed = false, trayVisible = false, ignoreTrayUp = false;
    int dpi = 96, scroll = 0, day = -1;
    ULONGLONG trialUntil = 0;

    ~Ui() {
        if(trialCancel) SetEvent(trialCancel);
        if(trialDeadline.joinable()) trialDeadline.join();
        if(healthCheck.valid()) healthCheck.wait();
        if(trialCancel) CloseHandle(trialCancel);
    }

    int s(int value) const { return MulDiv(value, dpi, 96); }
    void place(HWND control, int x, int y, int width, int height) const {
        MoveWindow(control, s(x), s(y), std::max(1,s(width)), std::max(1,s(height)), TRUE);
    }
    HWND control(HWND parent, const wchar_t* kind, const wchar_t* caption, DWORD style, int id = 0, DWORD ex = 0) {
        HWND result = CreateWindowExW(ex, kind, caption, WS_CHILD | WS_VISIBLE | style, 0,0,0,0,
                                    parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
        return result;
    }
    HWND label(const wchar_t* caption) {
        HWND result = control(settingsPane, L"STATIC", caption, SS_LEFT);
        settingsChildren.push_back(result);
        return result;
    }
    std::wstring loggingReason() const {
        return captureIssue.empty() ? historyIssue : captureIssue + (historyIssue.empty() ? L"" : L"\r\n" + historyIssue);
    }
    void statusDetails() {
        if(logControl) {
            const auto issue=loggingReason();
            SetWindowTextW(logControl,issue.empty() ? notice.c_str() : (L"기록 불가 · " + issue).c_str());
            layoutSettings();
        }
    }
    void addLog(const std::wstring& value) {
        if (value.empty()) return;
        notice = value;
        OutputDebugStringW((L"Toggle External Connection: " + value + L"\n").c_str());
        statusDetails();
        InvalidateRect(window, nullptr, FALSE);
    }
    void failure(const std::wstring& value) {
        addLog(value);
        MessageBoxW(window, value.c_str(), L"외부 연결 제어", MB_OK | MB_ICONERROR);
    }
    void updateTray() {
        if (preview) return;
        NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; icon.uCallbackMessage = trayMessage;
        icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(engine.active() ? 102 : 101));
        if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        std::wstring tooltip = engine.active() ? L"외부 연결 제어 · 차단 중" : L"외부 연결 제어 · 허용 중";
        if(engine.active() && !loggingReason().empty()) tooltip += L" · 기록 불가\n" + loggingReason();
        else tooltip += engine.active() ? L" (클릭하여 허용)" : L" (클릭하여 차단)";
        wcsncpy_s(icon.szTip, tooltip.c_str(), _TRUNCATE);
        trayVisible = Shell_NotifyIconW(trayVisible ? NIM_MODIFY : NIM_ADD, &icon) != FALSE;
    }
    void updateState() {
        const bool active = engine.active();
        SetWindowTextW(toggle, active ? L"외부 연결 허용" : L"외부 연결 차단");
        EnableWindow(toggle, !preview && (active || confirmed));
        EnableWindow(save, !preview && !active);
        EnableWindow(trial, !preview && confirmed && !active);
        EnableWindow(adaptersControl, !preview && !active);
        for (auto box : dev) EnableWindow(box,!preview && !active);
        EnableWindow(googleLogin,!preview && !active && SendMessageW(dev[4],BM_GETCHECK,0,0)==BST_CHECKED);
        SendMessageW(networks, EM_SETREADONLY, preview || active, 0);
        SendMessageW(dns, EM_SETREADONLY, preview || active, 0);
        statusDetails();
        updateTray();
        InvalidateRect(window, nullptr, TRUE);
    }
    int adapterIndex() const { return static_cast<int>(SendMessageW(adaptersControl, CB_GETCURSEL, 0, 0)); }
    void adapterDetails() {
        const int selected = adapterIndex();
        if (selected < 0 || selected >= static_cast<int>(adapters.size())) {
            SetWindowTextW(metadata, L"선택한 어댑터가 없습니다. 저장된 어댑터가 연결되어 있는지 확인하세요.");
            return;
        }
        SetWindowTextW(metadata,L"이 어댑터에서만 지정한 내부 주소를 허용합니다.\r\n다른 서브넷의 업무 서버가 있으면 아래에 추가하세요.");
    }
    void fillAdapter(bool useSaved) {
        const int selected = adapterIndex();
        if (selected < 0 || selected >= static_cast<int>(adapters.size())) { adapterDetails(); return; }
        const auto& a = adapters[selected];
        SetWindowTextW(networks, join(useSaved ? saved.networks : a.suggested.networks).c_str());
        SetWindowTextW(dns, join(useSaved ? saved.dns : a.suggested.dns).c_str());
        adapterDetails();
    }
    void loadAdapters() {
        std::wstring error;
        if (!preview) adapters = tec::adapters(error);
        else {
            tec::Adapter a; a.name = L"Ethernet — 사내망 (미리 보기)";
            a.guid = L"{60BC4767-8A69-41C8-9BE1-1A3E292E27E9}";
            a.suggested.luid = 1689399632855040ULL;
            a.suggested.networks = {L"192.168.10.0/24", L"fd12:3456:789a::/64"};
            a.suggested.dns = {L"192.168.10.1"}; adapters.push_back(a);
        }
        int selected = -1;
        for (size_t i = 0; i < adapters.size(); ++i) {
            SendMessageW(adaptersControl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(adapters[i].name.c_str()));
            if (_wcsicmp(adapters[i].guid.c_str(), saved.guid.c_str()) == 0) selected = static_cast<int>(i);
        }
        if (saved.guid.empty() && !adapters.empty()) selected = 0;
        SendMessageW(adaptersControl, CB_SETCURSEL, selected, 0);
        fillAdapter(!saved.guid.empty() && selected >= 0);
        if (!error.empty()) addLog(error);
        if (selected < 0) {
            confirmed = false;
            addLog(adapters.empty() ? L"사용 가능한 네트워크 어댑터가 없습니다." :
                                    L"저장된 어댑터를 찾지 못했습니다. 어댑터를 선택하고 다시 저장하세요.");
        }
    }
    void saveSettings() {
        if (preview || engine.active()) return;
        const int selected = adapterIndex();
        if (selected < 0 || selected >= static_cast<int>(adapters.size())) {
            failure(L"인트라넷을 사용할 어댑터를 선택하세요."); return;
        }
        tec::Settings candidate;
        candidate.guid = adapters[selected].guid;
        candidate.networks = split(text(networks)); candidate.dns = split(text(dns));
        for (unsigned i=0;i<5;++i) if (SendMessageW(dev[i],BM_GETCHECK,0,0)==BST_CHECKED) candidate.apps |= 1u<<i;
        const bool google=(candidate.apps&16) && SendMessageW(googleLogin,BM_GETCHECK,0,0)==BST_CHECKED;
        std::wstring error;
        if (!tec::validate(candidate, error)) { failure(error); return; }
        std::vector<std::wstring> apps;
        if (!tec::resolveApps(candidate.apps,apps,error)) { failure(error); return; }
        std::wstring summary = L"선택한 어댑터: " + adapters[selected].name + L"\n\n내부망 " +
            std::to_wstring(candidate.networks.size()) + L"개, DNS 서버 " + std::to_wstring(candidate.dns.size()) +
            L"개를 허용 목록으로 저장합니다.\n\n모든 어댑터의 외부 연결을 막고,\n"
            L"선택한 어댑터에서 지정한 내부 주소를 허용합니다.\n"
            L"DNS 서버 예외는 TCP/UDP 53번 포트에만 적용됩니다.";
        if (candidate.apps) {
            summary+=L"\n\n외부 접속 허용: ";
            for (unsigned i=0;i<5;++i) if (candidate.apps & (1u<<i)) summary+=std::wstring(devNames[i])+L"  ";
            if(google) summary+=L"\nGitHub 전용 MiniBrowser에서 Google 계정 로그인을 허용합니다.";
            if (!apps.empty()) summary+=L"\n확인한 앱 실행 파일 "+std::to_wstring(apps.size())+L"개 · 목적지 제한 없음";
        }
        summary+=L"\n\n이 설정을 저장할까요?";
        if (MessageBoxW(window, summary.c_str(), L"허용 범위 확인", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        if (!tec::saveSettings(dir, candidate, error)) { failure(error); return; }
        saved = std::move(candidate);
        DWORD result=tec::setGoogleLoginAllowed(google);
        if(result!=ERROR_SUCCESS) {
            confirmed=false; updateState();
            failure(L"앱 설정은 저장됐지만 Google 로그인 옵션을 저장하지 못했습니다. 다시 저장하세요. (Windows "+std::to_wstring(result)+L")");
            return;
        }
        confirmed = true;
        addLog(L"설정을 저장했습니다. 외부 연결 차단을 시작할 수 있습니다."); updateState();
    }
    void start(bool test) {
        if (preview || engine.active()) return;
        if (!confirmed) { show(false); failure(L"먼저 어댑터와 허용 범위를 확인하고 설정을 저장하세요."); return; }
        std::wstring error;
        const auto current = tec::adapters(error);
        const auto found = std::find_if(current.begin(), current.end(), [this](const tec::Adapter& a) {
            return _wcsicmp(a.guid.c_str(), saved.guid.c_str()) == 0;
        });
        if (found == current.end()) {
            failure(error.empty() ? L"저장된 어댑터가 없습니다. 연결 상태와 설정을 확인하세요." : error); return;
        }
        tec::Policy policy = found->suggested;
        policy.networks = saved.networks; policy.dns = saved.dns;
        if (!tec::resolveApps(saved.apps,policy.appPaths,error)) { failure(error); return; }
        if (saved.apps & 16) policy.webHelper=tec::installedExe();
        if(test && !trialCancel) trialCancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(test && !trialCancel) { failure(L"시험 차단의 자동 해제 타이머를 만들지 못했습니다."); return; }
        std::wstring warning;
        if (!engine.start(policy, [this](tec::Attempt attempt) { if (history) history->push(std::move(attempt)); }, error, warning)) {
            failure(error); updateState(); return;
        }
        ++generation;
        trialUntil = test ? GetTickCount64() + 60000 : 0;
        if (test) {
            if(trialDeadline.joinable()) trialDeadline.join();
            ResetEvent(trialCancel);
            const unsigned trialGeneration=generation;
            // The deadline also releases filters if the UI is waiting on disk or a system dialog.
            trialDeadline=std::thread([this,trialGeneration] {
                if(WaitForSingleObject(trialCancel,60000)==WAIT_TIMEOUT) {
                    engine.stop(); PostMessageW(window,trialEnded,trialGeneration,0);
                }
            });
            SetTimer(window, timerTrial, 250, nullptr);
        }
        captureIssue=warning;
        addLog(test ? L"60초 시험 차단을 시작했습니다. 시간이 끝나면 자동으로 허용합니다." : L"모든 어댑터의 외부 연결을 차단하고, 선택한 어댑터의 지정한 내부 주소를 허용합니다.");
        if (!warning.empty()) addLog(warning);
        updateState();
    }
    void stop(const wchar_t* reason = L"외부 연결을 허용합니다.") {
        ++generation;
        if(trialCancel) SetEvent(trialCancel);
        engine.stop();
        if(trialDeadline.joinable()) trialDeadline.join();
        KillTimer(window, timerTrial); trialUntil = 0;
        captureIssue.clear();
        if (history) history->flush();
        addLog(reason); updateState();
    }
    void toggleState() { if (engine.active()) stop(); else start(false); }
    void sampleHistory() {
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        const wchar_t* apps[] = {L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
                                L"C:\\Windows\\System32\\svchost.exe", L"C:\\Tools\\업무 도구\\업무.exe"};
        const wchar_t* ips[] = {L"203.0.113.18", L"2001:db8::42", L"198.51.100.24"};
        for (int i = 0; i < 3; ++i) {
            tec::Row r; r.first.time = now; r.last = now; r.first.path = apps[i]; r.first.ip = ips[i];
            r.first.port = i == 1 ? 443 : 80; r.first.protocol = i == 1 ? 17 : 6;
            r.count = 3 + i * 12; r.hint = i == 0 ? L"example.test (참고)" : L"—";
            visibleRows.push_back(std::move(r));
        }
    }
    void refreshHistory() {
        std::wstring selection = text(path);
        if (history) visibleRows = history->rows();
        else { visibleRows.clear(); if(preview) sampleHistory(); }
        const std::wstring app = lower(text(appSearch)), ip = lower(text(ipSearch));
        visibleRows.erase(std::remove_if(visibleRows.begin(), visibleRows.end(), [&](const tec::Row& row) {
            const auto application=row.first.path.empty() ? L"프로그램 미확인" : row.first.path;
            return lower(application).find(app) == std::wstring::npos || lower(row.first.ip).find(ip) == std::wstring::npos;
        }), visibleRows.end());
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        for (size_t i = 0; i < visibleRows.size(); ++i) {
            const auto& r = visibleRows[i];
            std::wstring values[] = {stamp(r.first.time), stamp(r.last), std::to_wstring(r.count), r.first.path.empty() ? L"프로그램 미확인" : leaf(r.first.path),
                r.first.ip + (r.first.port < 0 ? L"" : L":" + std::to_wstring(r.first.port)),
                r.first.protocol == 6 ? L"TCP" : r.first.protocol == 17 ? L"UDP" : std::to_wstring(r.first.protocol), r.hint};
            if (r.first.ip.find(L':') != std::wstring::npos && r.first.port >= 0)
                values[4] = L"[" + r.first.ip + L"]:" + std::to_wstring(r.first.port);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = &values[0][0];
            ListView_InsertItem(list, &item);
            for (int j = 1; j < 7; ++j) ListView_SetItemText(list, static_cast<int>(i), j, &values[j][0]);
            if ((!r.first.path.empty() && r.first.path==selection) || (r.first.path.empty() && selection==L"프로그램 미확인"))
                ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
        if (ListView_GetNextItem(list, -1, LVNI_SELECTED) < 0) SetWindowTextW(path, L"");
    }
    void show(bool historyView) {
        historyMode = historyView;
        ShowWindow(settingsPane, historyMode ? SW_HIDE : SW_SHOW);
        ShowWindow(historyPane, historyMode ? SW_SHOW : SW_HIDE);
        InvalidateRect(settingsNav, nullptr, TRUE); InvalidateRect(historyNav, nullptr, TRUE);
        layout(); if (historyMode) refreshHistory();
        ShowWindow(window, SW_SHOW); if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }
    void menu() {
        HMENU popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, navSettings, L"설정 열기");
        AppendMenuW(popup, MF_STRING, navHistory, L"차단 기록 보기");
        AppendMenuW(popup, MF_STRING | ((saved.apps&16) ? 0 : MF_GRAYED), webCommand, L"MiniBrowser에서 GitHub 열기");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING | (engine.active() || !confirmed ? MF_GRAYED : 0), trialCommand, L"60초 시험 차단");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, exitCommand, L"종료 · 연결 허용");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window);
        TrackPopupMenu(popup, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(popup); PostMessageW(window, WM_NULL, 0, 0);
    }
    void layoutSettings() {
        RECT client{}; GetClientRect(settingsPane, &client);
        int width = MulDiv(client.right, 96, dpi);
        RECT status{0,0,std::max(1,static_cast<int>(client.right)-s(36)),0};
        HDC dc=GetDC(logControl); SelectObject(dc,normalFont);
        const std::wstring statusText=text(logControl);
        DrawTextW(dc,statusText.c_str(),-1,&status,DT_CALCRECT|DT_WORDBREAK);
        ReleaseDC(logControl,dc);
        const int statusHeight=std::max(43,MulDiv(status.bottom,96,dpi));
        const int contentHeight=580+statusHeight+15;
        SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
        si.nMin = 0; si.nMax = s(contentHeight) - 1; si.nPage = client.bottom;
        scroll = std::max(0, std::min(scroll, std::max(0, s(contentHeight) - static_cast<int>(client.bottom))));
        si.nPos = scroll; SetScrollInfo(settingsPane, SB_VERT, &si, TRUE);
        GetClientRect(settingsPane, &client); width = MulDiv(client.right, 96, dpi);
        const int y = -MulDiv(scroll, 96, dpi), margin = 18, gap = 18;
        const int half = std::max(100, (width - 2 * margin - gap) / 2);
        place(settingsChildren[0], margin, y+16, width-36, 24);
        place(adaptersControl, margin, y+48, width-36, 230);
        place(metadata, margin, y+88, width-36, 42);
        place(settingsChildren[1], margin, y+147, half, 24);
        place(settingsChildren[2], margin+half+gap, y+147, half, 24);
        place(networks, margin, y+178, half, 142);
        place(dns, margin+half+gap, y+178, half, 142);
        place(settingsChildren[3], margin, y+328, half, 46);
        place(settingsChildren[4], margin+half+gap, y+328, half, 46);
        place(settingsChildren[6],margin,y+379,width-36,25);
        const int step=(width-36)/5;
        for (int i=0;i<5;++i) place(dev[i],margin+i*step,y+411,step-3,28);
        place(googleLogin,margin,y+444,width-36,28);
        place(save, margin, y+492, 150, 40); place(trial, margin+164, y+492, 170, 40);
        place(settingsChildren[5], margin, y+551, width-36, 24);
        place(logControl, margin, y+580, width-36, statusHeight);
    }
    void layout() {
        if (!window || !settingsPane) return;
        RECT r{}; GetClientRect(window, &r);
        const int w = MulDiv(r.right,96,dpi), h = MulDiv(r.bottom,96,dpi);
        place(settingsNav, 22, 102, 114, 36); place(historyNav, 144,102,126,36);
        place(toggle, w-210, 31,188,42);
        place(settingsPane,22,154,w-44,std::max(80,h-195));
        place(historyPane,22,154,w-44,std::max(80,h-195));
        layoutSettings();
        RECT hr{}; GetClientRect(historyPane,&hr); const int hw=MulDiv(hr.right,96,dpi), hh=MulDiv(hr.bottom,96,dpi);
        const bool compact=hh<220;
        const int filterTop=compact ? 31 : 43, listTop=compact ? 72 : 90;
        place(GetDlgItem(historyPane,2001),18,compact ? 8 : 16,200,22);
        place(GetDlgItem(historyPane,2002),hw/2,compact ? 8 : 16,180,22);
        place(appSearch,18,filterTop,hw/2-36,compact ? 28 : 31); place(ipSearch,hw/2,filterTop,hw/2-130,compact ? 28 : 31);
        place(GetDlgItem(historyPane,refreshButton),hw-112,filterTop,94,compact ? 28 : 31);
        const bool showPathLabel=hh>=230;
        ShowWindow(GetDlgItem(historyPane,2003),showPathLabel ? SW_SHOW : SW_HIDE);
        place(list,18,listTop,hw-36,std::max(20,hh-listTop-(showPathLabel ? 89 : 61)));
        place(GetDlgItem(historyPane,2003),18,hh-75,hw-36,22);
        place(path,18,hh-47,hw-36,29);
        InvalidateRect(window,nullptr,TRUE);
    }
};
Ui* ui;

void fonts(int dpi) {
    if (normalFont) DeleteObject(normalFont);
    if (headingFont) DeleteObject(headingFont);
    if (titleFont) DeleteObject(titleFont);
    normalFont = CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"맑은 고딕");
    headingFont = CreateFontW(-MulDiv(15,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"맑은 고딕");
    titleFont = CreateFontW(-MulDiv(23,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"맑은 고딕");
}

BOOL CALLBACK applyFont(HWND control, LPARAM) {
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(normalFont),TRUE); return TRUE;
}

void drawButton(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selectedNav = (item.CtlID == navSettings && !ui->historyMode) || (item.CtlID == navHistory && ui->historyMode);
    const bool primary = item.CtlID == toggleButton || item.CtlID == saveButton || selectedNav;
    COLORREF fill = primary && !disabled ? accent : panel;
    if (item.itemState & ODS_SELECTED) fill = RGB(44,83,133);
    HBRUSH brush = CreateSolidBrush(fill); FillRect(item.hDC,&item.rcItem,brush); DeleteObject(brush);
    SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,disabled ? RGB(125,132,143) : foreground);
    SelectObject(item.hDC,normalFont);
    RECT label = item.rcItem; const std::wstring caption = text(item.hwndItem);
    DrawTextW(item.hDC,caption.c_str(),-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if (item.itemState & ODS_FOCUS) {
        RECT outline = item.rcItem; InflateRect(&outline,-2,-2);
        HBRUSH ring = CreateSolidBrush(focus); FrameRect(item.hDC,&outline,ring); DeleteObject(ring);
    }
}

LRESULT commonColors(UINT message, WPARAM wp, LPARAM lp) {
    HDC dc = reinterpret_cast<HDC>(wp);
    SetTextColor(dc,foreground);
    COLORREF color = panel;
    HBRUSH brush = panelBrush;
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
        reinterpret_cast<HWND>(lp) == ui->networks || reinterpret_cast<HWND>(lp) == ui->dns ||
        reinterpret_cast<HWND>(lp) == ui->path) {
        color = input;
        brush = inputBrush;
    }
    SetBkColor(dc,color); return reinterpret_cast<LRESULT>(brush);
}

LRESULT CALLBACK paneProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: { RECT r{}; GetClientRect(hwnd,&r); FillRect(reinterpret_cast<HDC>(wp),&r,panelBrush); return 1; }
    case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: return commonColors(message,wp,lp);
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM: return SendMessageW(ui->window,message,wp,lp);
    case WM_VSCROLL:
        if (hwnd == ui->settingsPane) {
            SCROLLINFO si{sizeof(si),SIF_ALL}; GetScrollInfo(hwnd,SB_VERT,&si); int next=si.nPos;
            switch (LOWORD(wp)) {
            case SB_LINEUP: next-=ui->s(30); break; case SB_LINEDOWN: next+=ui->s(30); break;
            case SB_PAGEUP: next-=static_cast<int>(si.nPage); break; case SB_PAGEDOWN: next+=static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK: next=si.nTrackPos; break;
            }
            ui->scroll=next; ui->layoutSettings(); return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (hwnd == ui->settingsPane) {
            ui->scroll -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp),ui->s(60),WHEEL_DELTA); ui->layoutSettings(); return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

LRESULT CALLBACK headerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(hwnd,&paint); RECT client{}; GetClientRect(hwnd,&client);
        FillRect(dc,&client,panelBrush); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,foreground); SelectObject(dc,normalFont);
        for(int i=0;i<Header_GetItemCount(hwnd);++i) {
            RECT r{}; Header_GetItemRect(hwnd,i,&r); r.left+=ui->s(8); r.right-=ui->s(4);
            wchar_t caption[100]{}; HDITEMW item{}; item.mask=HDI_TEXT; item.pszText=caption; item.cchTextMax=100;
            Header_GetItem(hwnd,i,&item); DrawTextW(dc,caption,-1,&r,DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        EndPaint(hwnd,&paint); return 0;
    }
    if(message==WM_NCDESTROY) RemoveWindowSubclass(hwnd,headerProc,1);
    return DefSubclassProc(hwnd,message,wp,lp);
}

void createControls() {
    ui->settingsNav=ui->control(ui->window,L"BUTTON",L"설정",BS_OWNERDRAW|WS_TABSTOP,navSettings);
    ui->historyNav=ui->control(ui->window,L"BUTTON",L"차단 기록",BS_OWNERDRAW|WS_TABSTOP,navHistory);
    ui->toggle=ui->control(ui->window,L"BUTTON",L"외부 연결 차단",BS_OWNERDRAW|WS_TABSTOP,toggleButton);
    ui->settingsPane=ui->control(ui->window,L"TecPane",L"",WS_VSCROLL,0,WS_EX_CONTROLPARENT);
    ui->historyPane=ui->control(ui->window,L"TecPane",L"",0,0,WS_EX_CONTROLPARENT);
    SendMessageW(ui->label(L"인트라넷을 사용할 어댑터"),WM_SETFONT,reinterpret_cast<WPARAM>(headingFont),TRUE);
    ui->adaptersControl=ui->control(ui->settingsPane,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,adapterCombo);
    ui->metadata=ui->control(ui->settingsPane,L"STATIC",L"",SS_LEFT);
    ui->label(L"허용할 내부망 · IP, CIDR 또는 패턴");
    ui->label(L"허용할 DNS 서버 · 한 줄에 IP 하나");
    const DWORD multiline=WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL;
    ui->networks=ui->control(ui->settingsPane,L"EDIT",L"",multiline,networkEdit,WS_EX_CLIENTEDGE);
    ui->dns=ui->control(ui->settingsPane,L"EDIT",L"",multiline,dnsEdit,WS_EX_CLIENTEDGE);
    SendMessageW(ui->networks,EM_SETLIMITTEXT,16384,0); SendMessageW(ui->dns,EM_SETLIMITTEXT,16384,0);
    ui->label(L"예: 172.16.1.*, 172.16.*.*, 172.16.1.2?\r\n한 줄에 하나씩 입력하세요. ?는 숫자 한 자리입니다.");
    ui->label(L"DNS 예외는 TCP/UDP 53번 포트에만 적용됩니다.\r\n내부망과 DNS는 별도로 저장합니다.");
    ui->save=ui->control(ui->settingsPane,L"BUTTON",L"설정 확인 및 저장",BS_OWNERDRAW|BS_NOTIFY|WS_TABSTOP,saveButton);
    ui->trial=ui->control(ui->settingsPane,L"BUTTON",L"60초 시험 차단",BS_OWNERDRAW|BS_NOTIFY|WS_TABSTOP,trialButton);
    ui->label(L"현재 상태");
    ui->label(L"차단 중 외부 접속을 허용할 개발 앱");
    for (int i=0;i<5;++i) {
        ui->dev[i]=ui->control(ui->settingsPane,L"BUTTON",devNames[i],BS_AUTOCHECKBOX|BS_NOTIFY|WS_TABSTOP,firstDev+i);
        SetWindowTheme(ui->dev[i],L"",L"");
    }
    ui->googleLogin=ui->control(ui->settingsPane,L"BUTTON",L"GitHub 웹에서 Google 로그인 허용",BS_AUTOCHECKBOX|BS_NOTIFY|WS_TABSTOP,googleOption);
    SetWindowTheme(ui->googleLogin,L"",L"");
    ui->logControl=ui->control(ui->settingsPane,L"STATIC",L"",SS_LEFT);
    ui->control(ui->historyPane,L"STATIC",L"프로그램 이름 또는 전체 경로",0,2001);
    ui->control(ui->historyPane,L"STATIC",L"대상 IP",0,2002);
    ui->appSearch=ui->control(ui->historyPane,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,appFilter,WS_EX_CLIENTEDGE);
    ui->ipSearch=ui->control(ui->historyPane,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ipFilter,WS_EX_CLIENTEDGE);
    ui->control(ui->historyPane,L"BUTTON",L"새로 고침",BS_OWNERDRAW|WS_TABSTOP,refreshButton);
    ui->list=ui->control(ui->historyPane,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL|WS_TABSTOP,historyList,WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(ui->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(ui->list,editor); ListView_SetTextBkColor(ui->list,editor); ListView_SetTextColor(ui->list,foreground);
    SetWindowSubclass(ListView_GetHeader(ui->list),headerProc,1,0);
    const wchar_t* titles[]={L"최초 시각",L"최근 시각",L"횟수",L"프로그램",L"대상 IP:포트",L"프로토콜",L"이름 · 참고"};
    int widths[]={157,157,62,148,205,77,170};
    for (int i=0;i<7;++i) { LVCOLUMNW col{}; col.mask=LVCF_TEXT|LVCF_WIDTH; col.pszText=const_cast<wchar_t*>(titles[i]); col.cx=ui->s(widths[i]); ListView_InsertColumn(ui->list,i,&col); }
    ui->control(ui->historyPane,L"STATIC",L"선택한 프로그램 전체 경로 · 복사할 수 있습니다",0,2003);
    ui->path=ui->control(ui->historyPane,L"EDIT",L"",ES_AUTOHSCROLL|ES_READONLY|WS_TABSTOP,selectedPath,WS_EX_CLIENTEDGE);
    ShowWindow(ui->historyPane,SW_HIDE);
}

void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps); RECT r{}; GetClientRect(hwnd,&r); FillRect(dc,&r,backgroundBrush);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,foreground); SelectObject(dc,titleFont);
    RECT title{ui->s(22),ui->s(20),r.right-ui->s(235),ui->s(54)};
    const bool active=ui->engine.active();
    std::wstring status=ui->preview ? L"외부 연결 제어 · 미리 보기" : active ? L"외부 연결 차단 중" : L"외부 연결 허용 중";
    if(active && !ui->loggingReason().empty()) status=L"차단 중 · 기록 불가";
    DrawTextW(dc,status.c_str(),-1,&title,DT_SINGLELINE|DT_END_ELLIPSIS);
    SetTextColor(dc,muted); SelectObject(dc,normalFont);
    std::wstring detail=ui->preview ? L"UI 검증 모드 · 네트워크와 저장된 설정은 변경되지 않습니다" :
        active ? L"모든 외부 연결 차단 · 선택한 어댑터의 내부 주소만 허용" : L"트레이 아이콘을 한 번 클릭하면 차단 상태가 바뀝니다";
    if(active && !ui->loggingReason().empty()) detail=L"기록 불가: " + ui->loggingReason();
    if (ui->trialUntil) detail=L"시험 차단 · " + std::to_wstring(((ui->trialUntil>GetTickCount64() ? ui->trialUntil-GetTickCount64() : 0)+999)/1000) + L"초 후 자동 허용";
    RECT description{ui->s(22),ui->s(63),r.right-ui->s(22),ui->s(88)};
    DrawTextW(dc,detail.c_str(),-1,&description,DT_SINGLELINE|DT_END_ELLIPSIS);
    RECT footer{ui->s(22),r.bottom-ui->s(30),r.right-ui->s(22),r.bottom-ui->s(6)};
    const std::wstring foot=ui->preview ? L"미리 보기에서는 차단·저장·시험 기능을 사용할 수 없습니다." :
        ui->historyMode ? L"차단 시도 기록 · 이름은 참고 정보입니다" : L"닫으면 트레이로 숨김 · 종료하면 차단 해제";
    DrawTextW(dc,foot.c_str(),-1,&footer,DT_SINGLELINE|DT_END_ELLIPSIS);
    EndPaint(hwnd,&ps);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message==taskbarCreated && taskbarCreated) { ui->trayVisible=false; ui->updateTray(); return 0; }
    switch(message) {
    case WM_CREATE: ui->window=hwnd; createControls(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintWindow(hwnd); return 0;
    case WM_SIZE: ui->layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(lp);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
        limits->ptMinTrackSize.x=std::min(ui->s(720),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
        limits->ptMinTrackSize.y=std::min(ui->s(420),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top)); return 0;
    }
    case WM_DPICHANGED: {
        ui->dpi=HIWORD(wp); fonts(ui->dpi); EnumChildWindows(hwnd,applyFont,0);
        auto* r=reinterpret_cast<RECT*>(lp); SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        const int widths[]={157,157,62,148,205,77,170};
        for(int i=0;i<7;++i) ListView_SetColumnWidth(ui->list,i,ui->s(widths[i]));
        ui->layout(); return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: return commonColors(message,wp,lp);
    case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_COMMAND:
        if(lp && IsChild(ui->settingsPane,reinterpret_cast<HWND>(lp)) && GetFocus()==reinterpret_cast<HWND>(lp) &&
           (HIWORD(wp)==EN_SETFOCUS || HIWORD(wp)==BN_SETFOCUS || HIWORD(wp)==CBN_SETFOCUS)) {
            RECT item{},viewport{}; GetWindowRect(reinterpret_cast<HWND>(lp),&item); GetWindowRect(ui->settingsPane,&viewport);
            const int margin=ui->s(8);
            if(item.top<viewport.top+margin) ui->scroll+=item.top-viewport.top-margin;
            else if(item.bottom>viewport.bottom-margin) ui->scroll+=std::min(item.bottom-viewport.bottom+margin,item.top-viewport.top-margin);
            ui->layoutSettings();
        }
        if (LOWORD(wp)>=firstDev && LOWORD(wp)<=googleOption && HIWORD(wp)==BN_CLICKED) {
            if(LOWORD(wp)==lastDev && SendMessageW(ui->dev[4],BM_GETCHECK,0,0)!=BST_CHECKED)
                SendMessageW(ui->googleLogin,BM_SETCHECK,BST_UNCHECKED,0);
            ui->confirmed=false; ui->updateState(); return 0;
        }
        switch(LOWORD(wp)) {
        case navSettings: ui->show(false); return 0; case navHistory: ui->show(true); return 0;
        case toggleButton: ui->toggleState(); return 0;
        case saveButton: ui->saveSettings(); return 0;
        case trialButton: case trialCommand: ui->start(true); return 0;
        case exitCommand: DestroyWindow(hwnd); return 0;
        case adapterCombo:
            if(HIWORD(wp)==CBN_SELCHANGE) { ui->fillAdapter(false); ui->confirmed=false; ui->updateState(); } return 0;
        case networkEdit: case dnsEdit:
            if(HIWORD(wp)==EN_CHANGE && !ui->engine.active()) { ui->confirmed=false; ui->updateState(); } return 0;
        case appFilter: case ipFilter:
            if(HIWORD(wp)==EN_CHANGE) ui->refreshHistory(); return 0;
        case refreshButton: ui->refreshHistory(); return 0;
        case webCommand: {
            PWSTR programs=nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_CommonPrograms,0,nullptr,&programs))) return 0;
            std::wstring link=L"\""+std::wstring(programs)+L"\\Toggle External Connection\\GitHub Web.lnk\"";
            CoTaskMemFree(programs); wchar_t windows[MAX_PATH]{};
            GetWindowsDirectoryW(windows,MAX_PATH);
            const auto explorer=std::wstring(windows)+L"\\explorer.exe";
            ShellExecuteW(hwnd,L"open",explorer.c_str(),link.c_str(),nullptr,SW_SHOWNORMAL);
            return 0;
        }
        case IDCANCEL: SendMessageW(hwnd,WM_CLOSE,0,0); return 0;
        } break;
    case WM_NOTIFY: {
        auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if(hdr->hwndFrom==ui->list && hdr->code==LVN_ITEMCHANGED) {
            const int selected=ListView_GetNextItem(ui->list,-1,LVNI_SELECTED);
            const auto selectedRow=selected>=0 && selected<static_cast<int>(ui->visibleRows.size()) ? &ui->visibleRows[selected] : nullptr;
            SetWindowTextW(ui->path,selectedRow ? (selectedRow->first.path.empty() ? L"프로그램 미확인" : selectedRow->first.path.c_str()) : L"");
        }
        break;
    }
    case trayMessage:
        if(lp==WM_LBUTTONUP) {
            if(ui->ignoreTrayUp) ui->ignoreTrayUp=false;
            else SetTimer(hwnd,timerClick,GetDoubleClickTime()+30,nullptr);
        }
        else if(lp==WM_LBUTTONDBLCLK) { KillTimer(hwnd,timerClick); ui->ignoreTrayUp=true; ui->show(false); }
        else if(lp==WM_RBUTTONUP || lp==WM_CONTEXTMENU) { KillTimer(hwnd,timerClick); ui->menu(); }
        return 0;
    case trialEnded:
        if(static_cast<unsigned>(wp)==ui->generation)
            ui->stop(L"60초 시험 차단이 끝나 외부 연결을 허용합니다.");
        return 0;
    case WM_TIMER:
        if(wp==timerClick) { KillTimer(hwnd,timerClick); ui->toggleState(); }
        else if(wp==timerTrial) {
            if(GetTickCount64()>=ui->trialUntil) ui->stop(L"60초 시험 차단이 끝나 외부 연결을 허용합니다.");
            else InvalidateRect(hwnd,nullptr,FALSE);
        } else if(wp==timerRefresh) {
            if(!ui->preview && !ui->trayVisible) ui->updateTray();
            if(ui->healthCheck.valid() && ui->healthCheck.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
                const auto result=ui->healthCheck.get();
                if(result.first==ui->generation && ui->engine.active() && !result.second)
                    ui->stop(L"필터 상태를 확인할 수 없어 차단을 종료했습니다. 다시 시작하기 전에 상태를 확인하세요.");
            }
            if(ui->engine.active() && !ui->trialUntil && !ui->healthCheck.valid()) {
                const unsigned generation=ui->generation;
                ui->healthCheck=std::async(std::launch::async,[generation] { return std::make_pair(generation,ui->engine.healthy()); });
            }
            if(ui->history) {
                SYSTEMTIME today{}; GetLocalTime(&today);
                if(ui->day!=today.wDay && !ui->trialUntil) { ui->day=today.wDay; ui->history->maintain(); }
                const auto error=ui->history->error();
                if(error!=ui->historyIssue) { ui->historyIssue=error; ui->addLog(error); ui->updateState(); }
            }
            if(ui->historyMode && IsWindowVisible(hwnd)) ui->refreshHistory();
        } return 0;
    case WM_CLOSE:
        if(ui->preview) DestroyWindow(hwnd);
        else if(ui->trayVisible) ShowWindow(hwnd,SW_HIDE);
        else if(MessageBoxW(hwnd,L"트레이를 사용할 수 없습니다. 앱을 종료하고 외부 연결을 허용할까요?",
                L"외부 연결 제어",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)==IDYES) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        ++ui->generation;
        if(ui->trialCancel) SetEvent(ui->trialCancel);
        ui->engine.stop();
        if(ui->trialDeadline.joinable()) ui->trialDeadline.join();
        if(ui->healthCheck.valid()) ui->healthCheck.wait();
        if(ui->history) ui->history->flush();
        NOTIFYICONDATAW icon{}; icon.cbSize=sizeof(icon); icon.hWnd=hwnd; icon.uID=1;
        if(ui->trayVisible) Shell_NotifyIconW(NIM_DELETE,&icon);
        PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

bool isTrustedExecutable() {
    wchar_t module[32768]{};
    if(!GetModuleFileNameW(nullptr,module,32768)) return false;
    const std::wstring expected=tec::installedExe();
    if(expected.empty()) return false;
    std::wstring error;
    if(!tec::trustedInstallPath(expected,true,error)) return false;
    HANDLE file=CreateFileW(module,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    wchar_t resolved[32768]{}; const DWORD length=GetFinalPathNameByHandleW(file,resolved,32768,FILE_NAME_NORMALIZED); CloseHandle(file);
    if(!length || length>=32768) return false;
    std::wstring actual=resolved; if(actual.compare(0,4,L"\\\\?\\")==0) actual.erase(0,4);
    return _wcsicmp(actual.c_str(),expected.c_str())==0;
}

bool elevate(bool elevatedArgument) {
    if(!isTrustedExecutable()) {
        MessageBoxW(nullptr,L"일반 실행은 보호된 설치 경로에서만 가능합니다.\n설치 프로그램으로 설치한 뒤 실행하세요.\n\nUI만 확인하려면 --preview 또는 --preview-history를 사용하세요.",
                    L"안전한 설치 경로 필요",MB_OK|MB_ICONERROR); return false;
    }
    if(IsUserAnAdmin()) return true;
    if(elevatedArgument) { MessageBoxW(nullptr,L"관리자 권한을 얻지 못했습니다.",L"실행할 수 없음",MB_OK|MB_ICONERROR); return false; }
    const std::wstring exe=tec::installedExe();
    const std::wstring workingDirectory=exe.substr(0,exe.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW request{}; request.cbSize=sizeof(request); request.fMask=SEE_MASK_FLAG_NO_UI;
    request.lpVerb=L"runas"; request.lpFile=exe.c_str(); request.lpDirectory=workingDirectory.c_str();
    request.lpParameters=L"--elevated"; request.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&request)) {
        const DWORD code=GetLastError();
        MessageBoxW(nullptr,code==ERROR_CANCELLED ? L"관리자 권한 요청이 취소되어 실행을 마쳤습니다." : L"관리자 권한으로 시작하지 못했습니다. 설치 상태를 확인하세요.",
                    L"외부 연결 제어",MB_OK|(code==ERROR_CANCELLED ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    return false;
}
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int) {
    assert((split(L" 192.168.1.0/24 \r\n\r\n\tfd12::/64\n ") ==
            std::vector<std::wstring>{L"192.168.1.0/24",L"fd12::/64"}));
    assert(lower(L"C:\\Tools\\APP.EXE").find(L"app.exe") != std::wstring::npos);
    instance=app;
    int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv && argc==2 && wcscmp(argv[1],L"--github-web")==0) {LocalFree(argv);return tec::githubWindow();}
    Ui state; bool elevated=false;
    for(int i=1;argv && i<argc;++i) {
        if(wcscmp(argv[i],L"--preview")==0) state.preview=true;
        else if(wcscmp(argv[i],L"--preview-history")==0) { state.preview=true; state.historyMode=true; }
        else if(wcscmp(argv[i],L"--elevated")==0) elevated=true;
        else { LocalFree(argv); MessageBoxW(nullptr,L"지원하지 않는 실행 인수입니다.",L"외부 연결 제어",MB_OK|MB_ICONERROR); return 2; }
    }
    if(argv) LocalFree(argv);
    if(!state.preview && !elevate(elevated)) return 0;
    HANDLE singleton=nullptr;
    if(!state.preview) {
        singleton=CreateMutexW(nullptr,FALSE,L"Global\\ToggleExternalConnection.Production");
        const DWORD mutexError=GetLastError();
        if(!singleton || mutexError==ERROR_ALREADY_EXISTS) {
            if(singleton) CloseHandle(singleton);
            MessageBoxW(nullptr,mutexError==ERROR_ALREADY_EXISTS || mutexError==ERROR_ACCESS_DENIED ?
                L"이 컴퓨터의 다른 창 또는 사용자 세션에서 이미 실행 중입니다.\n실행 중인 세션의 트레이 아이콘을 사용하세요." :
                L"컴퓨터 전체의 실행 상태를 확인하지 못해 시작하지 않았습니다.",L"외부 연결 제어",MB_OK|MB_ICONINFORMATION); return 0;
        }
    }
    ui=&state;
    HMODULE user=GetModuleHandleW(L"user32.dll");
    auto perMonitor=reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(user,"SetProcessDpiAwarenessContext"));
    if(!perMonitor || !perMonitor(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) SetProcessDPIAware();
    HDC screen=GetDC(nullptr); state.dpi=GetDeviceCaps(screen,LOGPIXELSX); ReleaseDC(nullptr,screen);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    fonts(state.dpi); backgroundBrush=CreateSolidBrush(background); panelBrush=CreateSolidBrush(panel);
    inputBrush=CreateSolidBrush(input); editorBrush=CreateSolidBrush(editor);
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=instance; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.lpfnWndProc=paneProc; cls.lpszClassName=L"TecPane"; RegisterClassExW(&cls);
    cls.lpfnWndProc=windowProc; cls.lpszClassName=L"TecWindow";
    cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101)); cls.hIconSm=cls.hIcon; RegisterClassExW(&cls);
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int width=std::min(state.s(980),static_cast<int>(work.right-work.left)-state.s(20));
    const int height=std::min(state.s(850),static_cast<int>(work.bottom-work.top)-state.s(20));
    HWND window=CreateWindowExW(WS_EX_CONTROLPARENT,L"TecWindow",state.preview ? L"외부 연결 제어 — UI 미리 보기" : L"외부 연결 제어",
        WS_OVERLAPPEDWINDOW,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,
        nullptr,nullptr,instance,nullptr);
    if(!window) { if(singleton) CloseHandle(singleton); return 2; }
    if(!state.preview && taskbarCreated) ChangeWindowMessageFilterEx(window,taskbarCreated,MSGFLT_ALLOW,nullptr);
    if(!state.preview) {
        std::wstring error; state.dir=tec::storageDir(error);
        if(state.dir.empty()) { state.failure(error); DestroyWindow(window); }
        else {
            const bool loaded=tec::loadSettings(state.dir,state.saved,error);
            state.confirmed=loaded;
            if(!error.empty()) state.addLog(error);
            state.history.reset(new tec::History(state.dir));
            const auto historyError=state.history->error(); state.historyIssue=historyError; if(!historyError.empty()) state.addLog(historyError);
            state.loadAdapters();
            // Setting edit notifications invalidate confirmation while fields are populated.
            state.confirmed=loaded && state.adapterIndex()>=0;
            for(unsigned i=0;i<5;++i) SendMessageW(state.dev[i],BM_SETCHECK,(state.saved.apps&(1u<<i))?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(state.googleLogin,BM_SETCHECK,(state.saved.apps&16) && tec::googleLoginAllowed()?BST_CHECKED:BST_UNCHECKED,0);
            if(state.confirmed) state.addLog(L"저장된 인트라넷 설정을 사용합니다.");
        }
    } else { state.loadAdapters(); state.addLog(L"읽기 전용 UI 미리 보기입니다. 표시된 어댑터와 기록은 예시입니다."); }
    if(IsWindow(window)) {
        state.updateState(); state.show(state.historyMode); SetTimer(window,timerRefresh,2000,nullptr);
    }
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {
        if(!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    state.engine.stop(); if(state.history) state.history->flush(); state.history.reset();
    if(singleton) CloseHandle(singleton);
    DeleteObject(normalFont); DeleteObject(headingFont); DeleteObject(titleFont);
    DeleteObject(backgroundBrush); DeleteObject(panelBrush); DeleteObject(inputBrush); DeleteObject(editorBrush);
    return 0;
}
