#include "app.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cassert>
#include <cwctype>
#include <chrono>
#include <cstring>
#include <future>
#include <sstream>

namespace {
constexpr COLORREF background = RGB(26, 29, 34), panel = RGB(36, 40, 47);
constexpr COLORREF input = RGB(22, 25, 30), editor = RGB(20, 23, 28);
constexpr COLORREF border = RGB(58, 64, 76), buttonBg = RGB(47, 52, 62), buttonPress = RGB(38, 43, 52);
constexpr COLORREF foreground = RGB(238, 241, 246), muted = RGB(156, 165, 178), disabledText = RGB(114, 122, 134);
constexpr COLORREF accent = RGB(48, 108, 186), accentPress = RGB(38, 86, 150), focus = RGB(143, 193, 255);
constexpr COLORREF allowGreen = RGB(42, 138, 92), allowGreenPress = RGB(33, 112, 74);
constexpr COLORREF statusConnected = RGB(62, 184, 120), statusBlocked = RGB(224, 92, 82);
constexpr COLORREF statusWarning = RGB(235, 179, 64), statusPreview = RGB(108, 166, 232);
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT timerRefresh = 1, timerClick = 2;

enum {
    navSettings = 1001, navHistory, toggleButton, adapterCombo, refreshAdaptersButton,
    networkEdit, dnsEdit, saveButton, appFilter, ipFilter, clearFilterButton,
    historyList, selectedPath, copyPathButton, refreshButton,
    firstDev, lastDev = firstDev + 4, googleOption, webCommand, exportCsvButton, exitCommand = 1100
};
const wchar_t* devNames[] = {L"Codex", L"Claude", L"Antigravity", L"Git · gh", L"GitHub 웹"};
HINSTANCE instance;
UINT taskbarCreated;
HFONT normalFont, titleFont, headingFont;
HBRUSH backgroundBrush, panelBrush, inputBrush, editorBrush;

std::wstring text(HWND ctrl) {
    const int length = std::max(0, GetWindowTextLengthW(ctrl));
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(ctrl, &value[0], length + 1)));
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

bool sameFileTime(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

bool sameRowKey(const tec::Row& a, const tec::Row& b) {
    return sameFileTime(a.first.time, b.first.time) &&
           a.first.path == b.first.path && a.first.ip == b.first.ip &&
           a.first.port == b.first.port && a.first.protocol == b.first.protocol;
}

bool sameRows(const std::vector<tec::Row>& a, const std::vector<tec::Row>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameRowKey(a[i], b[i]) || !sameFileTime(a[i].last, b[i].last) ||
            a[i].count != b[i].count || a[i].hint != b[i].hint)
            return false;
    }
    return true;
}

std::wstring sampleNames(const std::vector<std::wstring>& paths, size_t limit = 3) {
    std::wstring summary;
    const size_t shown = std::min(paths.size(), limit);
    for (size_t i = 0; i < shown; ++i) {
        if (i) summary += L", ";
        summary += tec::historyProgram(paths[i]);
    }
    if (paths.size() > shown) summary += L" 외 " + std::to_wstring(paths.size() - shown) + L"개";
    return summary;
}

bool copyTextToClipboard(HWND owner, const std::wstring& value) {
    if (value.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) { CloseClipboard(); return false; }
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); CloseClipboard(); return false; }
    std::memcpy(dst, value.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); CloseClipboard(); return false; }
    CloseClipboard();
    return true;
}

struct Ui {
    HWND window{}, settingsPane{}, historyPane{};
    HWND settingsNav{}, historyNav{}, toggle{};
    HWND adapterHeading{}, adaptersControl{}, refreshAdapters{}, metadata{};
    HWND networkHeading{}, dnsHeading{}, networks{}, dns{}, networkHint{}, dnsHint{};
    HWND devHeading{}, dev[5]{}, googleLogin{}, appStatus{}, save{};
    HWND statusHeading{}, logControl{};
    HWND appFilterLabel{}, ipFilterLabel{}, appSearch{}, ipSearch{}, clearFilter{}, refreshHistoryBtn{}, exportCsvBtn{};
    HWND list{}, pathLabel{}, path{}, copyPathBtn{};
    std::vector<tec::Adapter> adapters;
    std::vector<tec::Row> visibleRows;
    size_t totalHistoryCount = 0;
    bool hasSelectedKey = false;
    tec::Row selectedKey{};
    std::wstring historyNotice;
    tec::Settings saved;
    tec::Engine engine;
    std::future<std::pair<unsigned, bool>> healthCheck;
    unsigned generation = 0;
    std::unique_ptr<tec::History> history;
    std::wstring dir, notice = L"설정을 확인하고 저장한 뒤 차단을 시작하세요.";
    std::wstring captureIssue, historyIssue;
    bool preview = false, historyMode = false, confirmed = false, unsaved = false;
    bool trayVisible = false, populating = false, appStatusWarning = false;
    ULONGLONG ignoreTrayUpUntil = 0;
    int dpi = 96, scroll = 0, day = -1;

    ~Ui() {
        if (healthCheck.valid()) healthCheck.wait();
    }

    int s(int value) const { return MulDiv(value, dpi, 96); }
    void place(HWND ctrl, int x, int y, int width, int height) const {
        MoveWindow(ctrl, s(x), s(y), std::max(1, s(width)), std::max(1, s(height)), TRUE);
    }
    HWND control(HWND parent, const wchar_t* kind, const wchar_t* caption, DWORD style, int id = 0, DWORD ex = 0) {
        HWND result = CreateWindowExW(ex, kind, caption, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
        return result;
    }
    void applyHeadingFonts() const {
        for (HWND heading : {adapterHeading, networkHeading, dnsHeading, devHeading, statusHeading}) {
            if (heading) SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
        }
    }
    std::wstring loggingReason() const {
        return captureIssue.empty() ? historyIssue : captureIssue + (historyIssue.empty() ? L"" : L"\r\n" + historyIssue);
    }
    unsigned checkedAppsMask() const {
        unsigned mask = 0;
        for (unsigned i = 0; i < 5; ++i)
            if (dev[i] && SendMessageW(dev[i], BM_GETCHECK, 0, 0) == BST_CHECKED) mask |= 1u << i;
        return mask;
    }
    void updateAppStatus() {
        if (!appStatus) return;
        const unsigned mask = checkedAppsMask();
        appStatusWarning = false;
        std::wstring summary;
        if (preview) {
            summary = L"체크한 앱의 실행 파일만 전체 경로 기준으로 허용합니다. (Antigravity 데스크톱·IDE·언어 서버·CLI·플러그인 포함)";
        } else if (!mask) {
            summary = L"체크한 앱의 실행 파일만 전체 경로 기준으로 외부 접속을 허용합니다.";
        } else {
            std::vector<std::wstring> foundParts, missingParts;
            for (unsigned i = 0; i < 4; ++i) {
                if (!(mask & (1u << i))) continue;
                std::vector<std::wstring> paths;
                std::wstring err;
                if (tec::resolveApps(1u << i, paths, err)) {
                    foundParts.push_back(std::wstring(devNames[i]) + L" " + std::to_wstring(paths.size()) +
                                         L"개 (" + sampleNames(paths, 2) + L")");
                } else {
                    missingParts.push_back(devNames[i]);
                }
            }
            if (mask & 16u) {
                const bool google = googleLogin && SendMessageW(googleLogin, BM_GETCHECK, 0, 0) == BST_CHECKED;
                foundParts.push_back(google ? L"GitHub 웹 (MiniBrowser · Google 로그인 포함)" : L"GitHub 웹 (MiniBrowser)");
            }
            if (!missingParts.empty()) {
                appStatusWarning = true;
                summary = L"실행 파일 미탐지: ";
                for (size_t i = 0; i < missingParts.size(); ++i) { if (i) summary += L", "; summary += missingParts[i]; }
                summary += L" — 설치 계정에서 앱을 먼저 열거나 예외를 해제하세요.";
                if (!foundParts.empty()) {
                    summary += L"\r\n탐지됨: ";
                    for (size_t i = 0; i < foundParts.size(); ++i) { if (i) summary += L" · "; summary += foundParts[i]; }
                }
            } else {
                summary = L"탐지됨: ";
                for (size_t i = 0; i < foundParts.size(); ++i) { if (i) summary += L" · "; summary += foundParts[i]; }
            }
        }
        SetWindowTextW(appStatus, summary.c_str());
        InvalidateRect(appStatus, nullptr, TRUE);
    }
    void statusDetails() {
        if (logControl) {
            const auto issue = loggingReason();
            const std::wstring textToShow = issue.empty()
                ? (unsaved ? std::wstring(L"설정이 변경되었습니다. '설정 확인 및 저장'을 눌러 저장해야 차단을 시작할 수 있습니다.") : notice)
                : (L"기록 불가 · " + issue);
            SetWindowTextW(logControl, textToShow.c_str());
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
    void markUnsaved() {
        if (populating || preview || engine.active()) return;
        confirmed = false;
        unsaved = true;
        updateAppStatus();
        updateState();
    }
    void updateTray() {
        if (preview) return;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon.uCallbackMessage = trayMessage;
        icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(engine.active() ? 102 : 101));
        if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        std::wstring tooltip = engine.active() ? L"외부 연결 제어 · 차단 중" : L"외부 연결 제어 · 허용 중";
        if (engine.active() && !loggingReason().empty()) tooltip += L" · 기록 불가\n" + loggingReason();
        else tooltip += engine.active() ? L" (클릭하여 허용)" : L" (클릭하여 차단)";
        wcsncpy_s(icon.szTip, tooltip.c_str(), _TRUNCATE);
        trayVisible = Shell_NotifyIconW(trayVisible ? NIM_MODIFY : NIM_ADD, &icon) != FALSE;
    }
    void updateState() {
        const bool active = engine.active();
        SetWindowTextW(toggle, active ? L"외부 연결 허용" : L"외부 연결 차단");
        EnableWindow(toggle, !preview && (active || confirmed));
        EnableWindow(save, !preview && !active);
        EnableWindow(adaptersControl, !preview && !active);
        EnableWindow(refreshAdapters, !preview && !active);
        for (auto box : dev) EnableWindow(box, !preview && !active);
        EnableWindow(googleLogin, !preview && !active && SendMessageW(dev[4], BM_GETCHECK, 0, 0) == BST_CHECKED);
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
        SetWindowTextW(metadata, L"이 어댑터에서만 지정한 내부 주소를 허용합니다. 다른 서브넷의 업무 서버가 있으면 아래에 추가하세요.");
    }
    void fillAdapter(bool useSaved) {
        const int selected = adapterIndex();
        if (selected < 0 || selected >= static_cast<int>(adapters.size())) { adapterDetails(); return; }
        const auto& a = adapters[selected];
        const bool wasPopulating = populating;
        populating = true;
        SetWindowTextW(networks, join(useSaved ? saved.networks : a.suggested.networks).c_str());
        SetWindowTextW(dns, join(useSaved ? saved.dns : a.suggested.dns).c_str());
        populating = wasPopulating;
        adapterDetails();
    }
    void loadAdapters(bool userInitiated = false) {
        std::wstring preferredGuid = saved.guid;
        const int previous = adapterIndex();
        if (userInitiated && previous >= 0 && previous < static_cast<int>(adapters.size()))
            preferredGuid = adapters[previous].guid;
        std::wstring error;
        adapters.clear();
        SendMessageW(adaptersControl, CB_RESETCONTENT, 0, 0);
        if (!preview) adapters = tec::adapters(error);
        else {
            tec::Adapter a;
            a.name = L"Ethernet — 사내망 (미리 보기)";
            a.guid = L"{60BC4767-8A69-41C8-9BE1-1A3E292E27E9}";
            a.suggested.luid = 1689399632855040ULL;
            a.suggested.networks = {L"192.168.10.0/24", L"fd12:3456:789a::/64"};
            a.suggested.dns = {L"192.168.10.1"};
            adapters.push_back(a);
        }
        int selected = -1;
        for (size_t i = 0; i < adapters.size(); ++i) {
            SendMessageW(adaptersControl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(adapters[i].name.c_str()));
            if (!preferredGuid.empty() && _wcsicmp(adapters[i].guid.c_str(), preferredGuid.c_str()) == 0)
                selected = static_cast<int>(i);
        }
        if (preferredGuid.empty() && !adapters.empty()) selected = 0;
        SendMessageW(adaptersControl, CB_SETCURSEL, selected, 0);
        if (!userInitiated) {
            fillAdapter(!saved.guid.empty() && selected >= 0);
        } else {
            adapterDetails();
            if (selected >= 0 && !saved.guid.empty() && _wcsicmp(adapters[selected].guid.c_str(), saved.guid.c_str()) != 0)
                markUnsaved();
            else if (selected < 0)
                confirmed = false;
            addLog(adapters.empty() ? L"사용 가능한 네트워크 어댑터가 없습니다."
                                    : L"네트워크 어댑터 목록을 새로 고쳤습니다 (" + std::to_wstring(adapters.size()) + L"개).");
        }
        if (!error.empty()) addLog(error);
        if (selected < 0 && !userInitiated) {
            confirmed = false;
            addLog(adapters.empty() ? L"사용 가능한 네트워크 어댑터가 없습니다."
                                    : L"저장된 어댑터를 찾지 못했습니다. 어댑터를 선택하고 다시 저장하세요.");
        }
    }
    void saveSettings() {
        if (preview || engine.active()) return;
        const int selected = adapterIndex();
        if (selected < 0 || selected >= static_cast<int>(adapters.size())) {
            failure(L"인트라넷을 사용할 어댑터를 선택하세요.");
            return;
        }
        tec::Settings candidate;
        candidate.guid = adapters[selected].guid;
        candidate.networks = split(text(networks));
        candidate.dns = split(text(dns));
        candidate.apps = checkedAppsMask();
        const bool google = (candidate.apps & 16u) && SendMessageW(googleLogin, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::wstring error;
        if (!tec::validate(candidate, error)) { failure(error); return; }
        std::vector<std::wstring> apps;
        if (!tec::resolveApps(candidate.apps, apps, error)) { updateAppStatus(); failure(error); return; }
        std::wstring summary = L"선택한 어댑터: " + adapters[selected].name + L"\n\n내부망 " +
            std::to_wstring(candidate.networks.size()) + L"개, DNS 서버 " + std::to_wstring(candidate.dns.size()) +
            L"개를 허용 목록으로 저장합니다.\n\n모든 어댑터의 외부 연결을 막고,\n"
            L"선택한 어댑터에서 지정한 내부 주소를 허용합니다.\n"
            L"DNS 서버 예외는 TCP/UDP 53번 포트에만 적용됩니다.";
        if (candidate.apps) {
            summary += L"\n\n외부 접속 허용: ";
            bool first = true;
            for (unsigned i = 0; i < 5; ++i) {
                if (!(candidate.apps & (1u << i))) continue;
                if (!first) summary += L", ";
                summary += devNames[i];
                first = false;
            }
            if (google) summary += L"\nGitHub 전용 MiniBrowser에서 Google 계정 로그인을 허용합니다.";
            if (!apps.empty())
                summary += L"\n확인한 실행 파일 " + std::to_wstring(apps.size()) + L"개 (" + sampleNames(apps, 4) + L") · 목적지 제한 없음";
        }
        summary += L"\n\n이 설정을 저장할까요?";
        if (MessageBoxW(window, summary.c_str(), L"허용 범위 확인", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        if (!tec::saveSettings(dir, candidate, error)) { failure(error); return; }
        saved = std::move(candidate);
        const DWORD result = tec::setGoogleLoginAllowed(google);
        if (result != ERROR_SUCCESS) {
            confirmed = false;
            unsaved = true;
            updateState();
            failure(L"앱 설정은 저장됐지만 Google 로그인 옵션을 저장하지 못했습니다. 다시 저장하세요. (Windows " + std::to_wstring(result) + L")");
            return;
        }
        confirmed = true;
        unsaved = false;
        updateAppStatus();
        addLog(L"설정을 저장했습니다. 외부 연결 차단을 시작할 수 있습니다.");
        updateState();
    }
    void start() {
        if (preview || engine.active()) return;
        if (!confirmed) { show(false); failure(L"먼저 어댑터와 허용 범위를 확인하고 설정을 저장하세요."); return; }
        std::wstring error;
        const auto current = tec::adapters(error);
        const auto found = std::find_if(current.begin(), current.end(), [this](const tec::Adapter& a) {
            return _wcsicmp(a.guid.c_str(), saved.guid.c_str()) == 0;
        });
        if (found == current.end()) {
            failure(error.empty() ? L"저장된 어댑터가 없습니다. 연결 상태와 설정을 확인하세요." : error);
            return;
        }
        tec::Policy policy = found->suggested;
        policy.networks = saved.networks;
        policy.dns = saved.dns;
        if (!tec::resolveApps(saved.apps, policy.appPaths, error)) { updateAppStatus(); failure(error); return; }
        if (saved.apps & 16u) policy.webHelper = tec::installedExe();
        std::wstring warning;
        if (!engine.start(policy, [this](tec::Attempt attempt) { if (history) history->push(std::move(attempt)); }, error, warning)) {
            failure(error);
            updateState();
            return;
        }
        ++generation;
        captureIssue = warning;
        addLog(L"모든 어댑터의 외부 연결을 차단하고, 선택한 어댑터의 지정한 내부 주소를 허용합니다.");
        if (!warning.empty()) addLog(warning);
        updateState();
    }
    void stop(const wchar_t* reason = L"외부 연결을 허용합니다.") {
        ++generation;
        engine.stop();
        captureIssue.clear();
        if (history) history->flush();
        addLog(reason);
        updateState();
    }
    void toggleState() { if (engine.active()) stop(); else start(); }
    void sampleHistory(std::vector<tec::Row>& out) const {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const wchar_t* apps[] = {L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
                                 L"C:\\Windows\\System32\\svchost.exe", L"C:\\Tools\\업무 도구\\업무.exe"};
        const wchar_t* ips[] = {L"203.0.113.18", L"2001:db8::42", L"198.51.100.24"};
        for (int i = 0; i < 3; ++i) {
            tec::Row r;
            r.first.time = now;
            r.last = now;
            r.first.path = apps[i];
            r.first.ip = ips[i];
            r.first.port = i == 1 ? 443 : 80;
            r.first.protocol = i == 1 ? 17 : 6;
            r.count = 3 + static_cast<unsigned long long>(i) * 12;
            r.hint = i == 0 ? L"example.test (참고)" : L"";
            out.push_back(std::move(r));
        }
    }
    void updateHistoryButtons() {
        const bool hasFilter = !text(appSearch).empty() || !text(ipSearch).empty();
        if (clearFilter) EnableWindow(clearFilter, hasFilter);
        if (exportCsvBtn) EnableWindow(exportCsvBtn, !preview && !visibleRows.empty());
        if (copyPathBtn) EnableWindow(copyPathBtn, !text(path).empty());
    }
    void updateSelectedPathField(const tec::Row* selectedRow) {
        const std::wstring nextText = selectedRow
            ? (selectedRow->first.path.empty() ? std::wstring(L"프로그램 미확인") : selectedRow->first.path)
            : std::wstring();
        if (text(path) != nextText) SetWindowTextW(path, nextText.c_str());
        if (copyPathBtn) EnableWindow(copyPathBtn, !nextText.empty());
    }
    void refreshHistory(bool force = false) {
        std::vector<tec::Row> allRows;
        if (history) allRows = history->rows();
        else if (preview) sampleHistory(allRows);
        totalHistoryCount = allRows.size();
        const std::wstring app = lower(text(appSearch)), ip = lower(text(ipSearch));
        allRows.erase(std::remove_if(allRows.begin(), allRows.end(), [&](const tec::Row& row) {
            const auto application = row.first.path.empty() ? L"프로그램 미확인" : row.first.path;
            return lower(application).find(app) == std::wstring::npos || lower(row.first.ip).find(ip) == std::wstring::npos;
        }), allRows.end());
        if (!force && sameRows(visibleRows, allRows)) {
            updateHistoryButtons();
            return;
        }
        const int prevTop = ListView_GetTopIndex(list);
        const int prevSelected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (prevSelected >= 0 && prevSelected < static_cast<int>(visibleRows.size())) {
            selectedKey = visibleRows[prevSelected];
            hasSelectedKey = true;
        }
        visibleRows = std::move(allRows);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        int restoredIndex = -1;
        for (size_t i = 0; i < visibleRows.size(); ++i) {
            const auto& r = visibleRows[i];
            std::wstring endpoint = r.first.ip + (r.first.port < 0 ? L"" : L":" + std::to_wstring(r.first.port));
            if (r.first.ip.find(L':') != std::wstring::npos && r.first.port >= 0)
                endpoint = L"[" + r.first.ip + L"]:" + std::to_wstring(r.first.port);
            std::wstring values[] = {
                tec::historyTime(r.first.time),
                tec::historyTime(r.last),
                std::to_wstring(r.count),
                tec::historyProgram(r.first.path),
                endpoint,
                tec::historyProtocol(r.first.protocol),
                r.hint.empty() ? std::wstring(L"—") : r.hint
            };
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = &values[0][0];
            ListView_InsertItem(list, &item);
            for (int j = 1; j < 7; ++j) ListView_SetItemText(list, static_cast<int>(i), j, &values[j][0]);
            if (hasSelectedKey && restoredIndex < 0 && sameRowKey(r, selectedKey)) {
                restoredIndex = static_cast<int>(i);
                ListView_SetItemState(list, restoredIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        if (prevTop > 0 && !visibleRows.empty()) {
            const int target = std::min(prevTop, static_cast<int>(visibleRows.size()) - 1);
            ListView_EnsureVisible(list, static_cast<int>(visibleRows.size()) - 1, FALSE);
            ListView_EnsureVisible(list, target, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        if (restoredIndex >= 0) {
            updateSelectedPathField(&visibleRows[restoredIndex]);
        } else {
            hasSelectedKey = false;
            updateSelectedPathField(nullptr);
        }
        updateHistoryButtons();
        InvalidateRect(window, nullptr, FALSE);
    }
    void exportHistory() {
        if (preview || visibleRows.empty()) return;
        // A modal Save As dialog still dispatches the timer that refreshes visibleRows.
        const auto snapshot = visibleRows;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t file[32768]{};
        swprintf_s(file, L"차단기록-%04u%02u%02u-%02u%02u%02u.csv", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        PWSTR documents = nullptr;
        SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrTitle = L"현재 차단 기록을 CSV로 저장";
        dialog.lpstrFile = file;
        dialog.nMaxFile = _countof(file);
        dialog.lpstrFilter = L"CSV 파일 (*.csv)\0*.csv\0";
        dialog.lpstrDefExt = L"csv";
        dialog.lpstrInitialDir = documents;
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_DONTADDTORECENT | OFN_NOTESTFILECREATE;
        const BOOL picked = GetSaveFileNameW(&dialog);
        const DWORD dialogError = picked ? 0 : CommDlgExtendedError();
        CoTaskMemFree(documents);
        if (!picked) {
            if (dialogError) failure(L"파일 저장 창을 열지 못했습니다. (Windows " + std::to_wstring(dialogError) + L")");
            return;
        }
        const DWORD error = tec::saveHistoryCsv(file, snapshot);
        if (error) {
            if (error == ERROR_INVALID_NAME) failure(L"파일 이름의 확장자를 .csv로 지정하세요.");
            else failure(L"CSV를 저장하지 못했습니다. 파일이 열려 있는지, 저장 위치에 쓸 수 있는지 확인하세요. (Windows " + std::to_wstring(error) + L")");
            return;
        }
        historyNotice = L"CSV 저장 완료 · " + std::to_wstring(snapshot.size()) + L"행 · " + (file + dialog.nFileOffset);
        InvalidateRect(window, nullptr, FALSE);
    }
    void show(bool historyView) {
        historyMode = historyView;
        ShowWindow(settingsPane, historyMode ? SW_HIDE : SW_SHOW);
        ShowWindow(historyPane, historyMode ? SW_SHOW : SW_HIDE);
        InvalidateRect(settingsNav, nullptr, TRUE);
        InvalidateRect(historyNav, nullptr, TRUE);
        layout();
        if (historyMode) refreshHistory(true);
        else { updateAppStatus(); layoutSettings(); }
        ShowWindow(window, SW_SHOW);
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }
    void menu() {
        HMENU popup = CreatePopupMenu();
        const bool active = engine.active();
        const wchar_t* toggleLabel = active ? L"외부 연결 허용 (현재: 차단 중)" : L"외부 연결 차단 (현재: 허용 중)";
        AppendMenuW(popup, MF_STRING | ((!preview && (active || confirmed)) ? 0 : MF_GRAYED), toggleButton, toggleLabel);
        SetMenuDefaultItem(popup, 0, TRUE);
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, navSettings, L"설정 열기");
        AppendMenuW(popup, MF_STRING, navHistory, L"차단 기록 보기");
        AppendMenuW(popup, MF_STRING | ((saved.apps & 16u) ? 0 : MF_GRAYED), webCommand, L"MiniBrowser에서 GitHub 열기");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, exitCommand, L"종료 · 연결 허용");
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        TrackPopupMenu(popup, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(popup);
        PostMessageW(window, WM_NULL, 0, 0);
    }
    int measureTextHeight(HWND ctrl, int widthPx, int minHeightDip) const {
        RECT box{0, 0, std::max(1, widthPx), 0};
        HDC dc = GetDC(ctrl);
        HGDIOBJ oldFont = SelectObject(dc, normalFont);
        const std::wstring content = text(ctrl);
        DrawTextW(dc, content.c_str(), -1, &box, DT_CALCRECT | DT_WORDBREAK);
        SelectObject(dc, oldFont);
        ReleaseDC(ctrl, dc);
        return std::max(minHeightDip, MulDiv(box.bottom, 96, dpi) + 4);
    }
    void layoutSettings() {
        RECT client{};
        GetClientRect(settingsPane, &client);
        int width = MulDiv(client.right, 96, dpi);
        const int innerPx = std::max(1, static_cast<int>(client.right) - s(36));
        const bool twoRowDev = width < 760;
        const int devRowsHeight = twoRowDev ? 62 : 30;
        const int appStatusHeight = measureTextHeight(appStatus, innerPx, 24);
        const int statusHeight = measureTextHeight(logControl, innerPx, 42);
        const int devTop = 382;
        const int googleTop = devTop + 30 + devRowsHeight + 4;
        const int appStatusTop = googleTop + 32;
        const int saveTop = appStatusTop + appStatusHeight + 14;
        const int statusHeadingTop = saveTop + 56;
        const int logTop = statusHeadingTop + 28;
        const int contentHeight = logTop + statusHeight + 18;

        SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
        si.nMin = 0;
        si.nMax = s(contentHeight) - 1;
        si.nPage = static_cast<UINT>(client.bottom);
        scroll = std::max(0, std::min(scroll, std::max(0, s(contentHeight) - static_cast<int>(client.bottom))));
        si.nPos = scroll;
        SetScrollInfo(settingsPane, SB_VERT, &si, TRUE);

        GetClientRect(settingsPane, &client);
        width = MulDiv(client.right, 96, dpi);
        const int y = -MulDiv(scroll, 96, dpi), margin = 18, gap = 18;
        const int contentW = std::max(240, width - 2 * margin);
        const int half = std::max(100, (contentW - gap) / 2);
        const int refreshBtnW = 122;

        place(adapterHeading, margin, y + 16, contentW, 24);
        place(adaptersControl, margin, y + 46, std::max(120, contentW - refreshBtnW - 10), 230);
        place(refreshAdapters, margin + contentW - refreshBtnW, y + 46, refreshBtnW, 28);
        place(metadata, margin, y + 82, contentW, 38);

        place(networkHeading, margin, y + 132, half, 24);
        place(dnsHeading, margin + half + gap, y + 132, half, 24);
        place(networks, margin, y + 162, half, 146);
        place(dns, margin + half + gap, y + 162, half, 146);
        place(networkHint, margin, y + 314, half, 44);
        place(dnsHint, margin + half + gap, y + 314, half, 44);

        place(devHeading, margin, y + devTop, contentW, 24);
        if (twoRowDev) {
            const int colW = contentW / 3;
            for (int i = 0; i < 3; ++i) place(dev[i], margin + i * colW, y + devTop + 30, colW - 6, 28);
            for (int i = 3; i < 5; ++i) place(dev[i], margin + (i - 3) * colW, y + devTop + 62, colW - 6, 28);
        } else {
            const int weights[5] = {15, 15, 24, 21, 25};
            int x = margin;
            for (int i = 0; i < 5; ++i) {
                const int w = (i == 4) ? (margin + contentW - x) : (contentW * weights[i] / 100);
                place(dev[i], x, y + devTop + 30, w - 6, 28);
                x += w;
            }
        }
        place(googleLogin, margin, y + googleTop, contentW, 28);
        place(appStatus, margin, y + appStatusTop, contentW, appStatusHeight);
        place(save, margin, y + saveTop, 168, 40);
        place(statusHeading, margin, y + statusHeadingTop, contentW, 24);
        place(logControl, margin, y + logTop, contentW, statusHeight);
    }
    void layout() {
        if (!window || !settingsPane) return;
        RECT r{};
        GetClientRect(window, &r);
        const int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
        place(settingsNav, 22, 102, 114, 36);
        place(historyNav, 144, 102, 126, 36);
        place(toggle, w - 210, 28, 188, 44);
        place(settingsPane, 22, 152, w - 44, std::max(80, h - 192));
        place(historyPane, 22, 152, w - 44, std::max(80, h - 192));
        layoutSettings();

        RECT hr{};
        GetClientRect(historyPane, &hr);
        const int hw = MulDiv(hr.right, 96, dpi), hh = MulDiv(hr.bottom, 96, dpi);
        const bool compact = hh < 220;
        const int filterTop = compact ? 31 : 43, listTop = compact ? 72 : 90;
        const int actionTotal = 72 + 8 + 84 + 8 + 92 + 12;
        const int filterWidth = std::max(60, (hw - 36 - actionTotal - 12) / 2);
        const int ipLeft = 18 + filterWidth + 12, filterHeight = compact ? 28 : 31;
        place(appFilterLabel, 18, compact ? 8 : 16, filterWidth, 22);
        place(ipFilterLabel, ipLeft, compact ? 8 : 16, filterWidth, 22);
        place(appSearch, 18, filterTop, filterWidth, filterHeight);
        place(ipSearch, ipLeft, filterTop, filterWidth, filterHeight);
        place(clearFilter, hw - 282, filterTop, 72, filterHeight);
        place(refreshHistoryBtn, hw - 202, filterTop, 84, filterHeight);
        place(exportCsvBtn, hw - 110, filterTop, 92, filterHeight);
        const bool showPathLabel = hh >= 230;
        ShowWindow(pathLabel, showPathLabel ? SW_SHOW : SW_HIDE);
        place(list, 18, listTop, hw - 36, std::max(20, hh - listTop - (showPathLabel ? 89 : 61)));
        place(pathLabel, 18, hh - 75, hw - 36, 22);
        const int copyW = 92;
        place(path, 18, hh - 47, std::max(80, hw - 36 - copyW - 8), 29);
        place(copyPathBtn, hw - 18 - copyW, hh - 47, copyW, 29);
        InvalidateRect(window, nullptr, TRUE);
    }
};
Ui* ui;

void fonts(int dpi) {
    if (normalFont) DeleteObject(normalFont);
    if (headingFont) DeleteObject(headingFont);
    if (titleFont) DeleteObject(titleFont);
    normalFont = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    headingFont = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    titleFont = CreateFontW(-MulDiv(23, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
}

BOOL CALLBACK applyFont(HWND control, LPARAM) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
    return TRUE;
}

void drawButton(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool isNav = item.CtlID == navSettings || item.CtlID == navHistory;
    const bool selectedNav = (item.CtlID == navSettings && !ui->historyMode) || (item.CtlID == navHistory && ui->historyMode);
    const bool activeToggle = item.CtlID == toggleButton && ui->engine.active();
    const bool primarySave = item.CtlID == saveButton && (!ui->confirmed || ui->unsaved);
    const bool primaryToggle = item.CtlID == toggleButton;

    COLORREF fill = buttonBg;
    COLORREF outlineColor = border;
    if (isNav) {
        fill = selectedNav ? panel : background;
        outlineColor = selectedNav ? border : RGB(44, 49, 58);
    } else if (!disabled && activeToggle) {
        fill = pressed ? allowGreenPress : allowGreen;
        outlineColor = RGB(66, 174, 122);
    } else if (!disabled && (primaryToggle || primarySave)) {
        fill = pressed ? accentPress : accent;
        outlineColor = RGB(82, 146, 224);
    } else if (pressed && !disabled) {
        fill = buttonPress;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);

    HBRUSH frame = CreateSolidBrush(outlineColor);
    FrameRect(item.hDC, &item.rcItem, frame);
    DeleteObject(frame);

    if (selectedNav) {
        RECT bar = item.rcItem;
        bar.top = std::max(bar.top, bar.bottom - ui->s(3));
        HBRUSH barBrush = CreateSolidBrush(RGB(92, 158, 240));
        FillRect(item.hDC, &bar, barBrush);
        DeleteObject(barBrush);
    }

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? disabledText : (isNav && !selectedNav ? muted : foreground));
    HGDIOBJ oldFont = SelectObject(item.hDC, normalFont);
    RECT label = item.rcItem;
    const std::wstring caption = text(item.hwndItem);
    DrawTextW(item.hDC, caption.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, oldFont);

    if (item.itemState & ODS_FOCUS) {
        RECT outline = item.rcItem;
        InflateRect(&outline, -2, -2);
        HBRUSH ring = CreateSolidBrush(focus);
        FrameRect(item.hDC, &outline, ring);
        DeleteObject(ring);
    }
}

LRESULT commonColors(UINT message, WPARAM wp, LPARAM lp) {
    HDC dc = reinterpret_cast<HDC>(wp);
    HWND target = reinterpret_cast<HWND>(lp);
    COLORREF textColor = foreground;
    COLORREF bgColor = panel;
    HBRUSH brush = panelBrush;
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
        target == ui->networks || target == ui->dns || target == ui->path) {
        bgColor = input;
        brush = inputBrush;
    } else if (target == ui->metadata || target == ui->networkHint || target == ui->dnsHint ||
               target == ui->appFilterLabel || target == ui->ipFilterLabel || target == ui->pathLabel) {
        textColor = muted;
    } else if (target == ui->appStatus) {
        textColor = ui->appStatusWarning ? statusWarning : muted;
    } else if (target == ui->logControl) {
        if (!ui->loggingReason().empty() || ui->unsaved) textColor = statusWarning;
    }
    SetTextColor(dc, textColor);
    SetBkColor(dc, bgColor);
    return reinterpret_cast<LRESULT>(brush);
}

LRESULT CALLBACK paneProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect(reinterpret_cast<HDC>(wp), &r, panelBrush);
        HBRUSH frame = CreateSolidBrush(border);
        FrameRect(reinterpret_cast<HDC>(wp), &r, frame);
        DeleteObject(frame);
        return 1;
    }
    case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return commonColors(message, wp, lp);
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM:
        return SendMessageW(ui->window, message, wp, lp);
    case WM_VSCROLL:
        if (hwnd == ui->settingsPane) {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(hwnd, SB_VERT, &si);
            int next = si.nPos;
            switch (LOWORD(wp)) {
            case SB_LINEUP: next -= ui->s(30); break;
            case SB_LINEDOWN: next += ui->s(30); break;
            case SB_PAGEUP: next -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN: next += static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK: next = si.nTrackPos; break;
            }
            ui->scroll = next;
            ui->layoutSettings();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (hwnd == ui->settingsPane) {
            ui->scroll -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp), ui->s(60), WHEEL_DELTA);
            ui->layoutSettings();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK headerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, panelBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);
        HGDIOBJ oldFont = SelectObject(dc, normalFont);
        for (int i = 0; i < Header_GetItemCount(hwnd); ++i) {
            RECT r{};
            Header_GetItemRect(hwnd, i, &r);
            r.left += ui->s(8);
            r.right -= ui->s(4);
            wchar_t caption[100]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = caption;
            item.cchTextMax = 100;
            Header_GetItem(hwnd, i, &item);
            DrawTextW(dc, caption, -1, &r, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, headerProc, 1);
    return DefSubclassProc(hwnd, message, wp, lp);
}

void createControls() {
    ui->settingsNav = ui->control(ui->window, L"BUTTON", L"설정", BS_OWNERDRAW | WS_TABSTOP, navSettings);
    ui->historyNav = ui->control(ui->window, L"BUTTON", L"차단 기록", BS_OWNERDRAW | WS_TABSTOP, navHistory);
    ui->toggle = ui->control(ui->window, L"BUTTON", L"외부 연결 차단", BS_OWNERDRAW | WS_TABSTOP, toggleButton);
    ui->settingsPane = ui->control(ui->window, L"TecPane", L"", WS_VSCROLL, 0, WS_EX_CONTROLPARENT);
    ui->historyPane = ui->control(ui->window, L"TecPane", L"", 0, 0, WS_EX_CONTROLPARENT);

    ui->adapterHeading = ui->control(ui->settingsPane, L"STATIC", L"인트라넷을 사용할 어댑터", SS_LEFT);
    ui->adaptersControl = ui->control(ui->settingsPane, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, adapterCombo);
    ui->refreshAdapters = ui->control(ui->settingsPane, L"BUTTON", L"어댑터 새로 고침", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, refreshAdaptersButton);
    ui->metadata = ui->control(ui->settingsPane, L"STATIC", L"", SS_LEFT);

    ui->networkHeading = ui->control(ui->settingsPane, L"STATIC", L"허용할 내부망 · IP, CIDR 또는 패턴", SS_LEFT);
    ui->dnsHeading = ui->control(ui->settingsPane, L"STATIC", L"허용할 DNS 서버 · 한 줄에 IP 하나", SS_LEFT);
    const DWORD multiline = WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
    ui->networks = ui->control(ui->settingsPane, L"EDIT", L"", multiline, networkEdit, WS_EX_CLIENTEDGE);
    ui->dns = ui->control(ui->settingsPane, L"EDIT", L"", multiline, dnsEdit, WS_EX_CLIENTEDGE);
    SendMessageW(ui->networks, EM_SETLIMITTEXT, 16384, 0);
    SendMessageW(ui->dns, EM_SETLIMITTEXT, 16384, 0);
    ui->networkHint = ui->control(ui->settingsPane, L"STATIC", L"예: 172.16.1.*, 172.16.*.*, 172.16.1.2?\r\n한 줄에 하나씩 입력하세요. ?는 숫자 한 자리입니다.", SS_LEFT);
    ui->dnsHint = ui->control(ui->settingsPane, L"STATIC", L"DNS 예외는 TCP/UDP 53번 포트에만 적용됩니다.\r\n내부망과 DNS는 별도로 저장합니다.", SS_LEFT);

    ui->devHeading = ui->control(ui->settingsPane, L"STATIC", L"차단 중 외부 접속을 허용할 개발 앱", SS_LEFT);
    for (int i = 0; i < 5; ++i) {
        ui->dev[i] = ui->control(ui->settingsPane, L"BUTTON", devNames[i], BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, firstDev + i);
        SetWindowTheme(ui->dev[i], L"", L"");
    }
    ui->googleLogin = ui->control(ui->settingsPane, L"BUTTON", L"GitHub 웹에서 Google 로그인 허용", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, googleOption);
    SetWindowTheme(ui->googleLogin, L"", L"");
    ui->appStatus = ui->control(ui->settingsPane, L"STATIC", L"", SS_LEFT);
    ui->save = ui->control(ui->settingsPane, L"BUTTON", L"설정 확인 및 저장", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, saveButton);
    ui->statusHeading = ui->control(ui->settingsPane, L"STATIC", L"현재 상태", SS_LEFT);
    ui->logControl = ui->control(ui->settingsPane, L"STATIC", L"", SS_LEFT);
    ui->applyHeadingFonts();

    ui->appFilterLabel = ui->control(ui->historyPane, L"STATIC", L"프로그램 이름 또는 전체 경로", 0);
    ui->ipFilterLabel = ui->control(ui->historyPane, L"STATIC", L"대상 IP", 0);
    ui->appSearch = ui->control(ui->historyPane, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, appFilter, WS_EX_CLIENTEDGE);
    ui->ipSearch = ui->control(ui->historyPane, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ipFilter, WS_EX_CLIENTEDGE);
    ui->clearFilter = ui->control(ui->historyPane, L"BUTTON", L"초기화", BS_OWNERDRAW | WS_TABSTOP, clearFilterButton);
    ui->refreshHistoryBtn = ui->control(ui->historyPane, L"BUTTON", L"새로 고침", BS_OWNERDRAW | WS_TABSTOP, refreshButton);
    ui->exportCsvBtn = ui->control(ui->historyPane, L"BUTTON", L"CSV 저장", BS_OWNERDRAW | WS_TABSTOP, exportCsvButton);
    ui->list = ui->control(ui->historyPane, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, historyList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(ui->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(ui->list, editor);
    ListView_SetTextBkColor(ui->list, editor);
    ListView_SetTextColor(ui->list, foreground);
    SetWindowSubclass(ListView_GetHeader(ui->list), headerProc, 1, 0);
    const wchar_t* titles[] = {L"최초 시각", L"최근 시각", L"횟수", L"프로그램", L"대상 IP:포트", L"프로토콜", L"이름 · 참고"};
    int widths[] = {157, 157, 62, 148, 205, 77, 170};
    for (int i = 0; i < 7; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(titles[i]);
        col.cx = ui->s(widths[i]);
        ListView_InsertColumn(ui->list, i, &col);
    }
    ui->pathLabel = ui->control(ui->historyPane, L"STATIC", L"선택한 프로그램 전체 경로 · 복사할 수 있습니다", 0);
    ui->path = ui->control(ui->historyPane, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, selectedPath, WS_EX_CLIENTEDGE);
    ui->copyPathBtn = ui->control(ui->historyPane, L"BUTTON", L"경로 복사", BS_OWNERDRAW | WS_TABSTOP, copyPathButton);
    ShowWindow(ui->historyPane, SW_HIDE);
}

void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r{};
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, backgroundBrush);

    const bool active = ui->engine.active();
    const bool hasLogIssue = active && !ui->loggingReason().empty();
    const COLORREF dotColor = ui->preview ? statusPreview : (hasLogIssue ? statusWarning : (active ? statusBlocked : statusConnected));

    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, dotColor);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, dotPen);
    Ellipse(dc, ui->s(22), ui->s(31), ui->s(34), ui->s(43));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(dotBrush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    HGDIOBJ oldFont = SelectObject(dc, titleFont);
    RECT title{ui->s(44), ui->s(20), r.right - ui->s(235), ui->s(54)};
    std::wstring status = ui->preview ? L"외부 연결 제어 · 미리 보기" : active ? L"외부 연결 차단 중" : L"외부 연결 허용 중";
    if (hasLogIssue) status = L"차단 중 · 기록 불가";
    DrawTextW(dc, status.c_str(), -1, &title, DT_SINGLELINE | DT_END_ELLIPSIS);

    SetTextColor(dc, hasLogIssue ? statusWarning : muted);
    SelectObject(dc, normalFont);
    std::wstring detail = ui->preview ? L"UI 검증 모드 · 네트워크와 저장된 설정은 변경되지 않습니다" :
        active ? L"외부 연결 차단 중 · 트레이 아이콘을 다시 누르면 해제됩니다" :
        (ui->unsaved ? L"변경된 설정을 저장하면 외부 연결 차단을 시작할 수 있습니다" : L"트레이 아이콘을 한 번 클릭하면 차단 상태가 바뀝니다");
    if (hasLogIssue) detail = L"기록 불가: " + ui->loggingReason();
    RECT description{ui->s(22), ui->s(63), r.right - ui->s(235), ui->s(88)};
    DrawTextW(dc, detail.c_str(), -1, &description, DT_SINGLELINE | DT_END_ELLIPSIS);

    SetTextColor(dc, muted);
    RECT footer{ui->s(22), r.bottom - ui->s(30), r.right - ui->s(22), r.bottom - ui->s(6)};
    std::wstring foot;
    if (ui->preview) {
        foot = L"미리 보기에서는 차단·저장 기능을 사용할 수 없습니다.";
    } else if (ui->historyMode) {
        const std::wstring countText = ui->visibleRows.size() == ui->totalHistoryCount
            ? (L"표시 중 " + std::to_wstring(ui->visibleRows.size()) + L"건")
            : (L"검색 결과 " + std::to_wstring(ui->visibleRows.size()) + L"건 / 전체 " + std::to_wstring(ui->totalHistoryCount) + L"건");
        foot = countText + L" · " + (ui->historyNotice.empty() ? L"CSV 저장은 현재 검색 결과를 내보냅니다." : ui->historyNotice);
    } else {
        foot = L"닫으면 트레이로 숨김 · 종료하면 차단 해제";
    }
    DrawTextW(dc, foot.c_str(), -1, &footer, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

bool isFocusNotification(HWND control, WORD code) {
    if (control == ui->networks || control == ui->dns) return code == EN_SETFOCUS;
    if (control == ui->adaptersControl) return code == CBN_SETFOCUS;
    if (control == ui->save || control == ui->refreshAdapters || control == ui->googleLogin) return code == BN_SETFOCUS;
    for (HWND box : ui->dev) if (control == box) return code == BN_SETFOCUS;
    return false;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == taskbarCreated && taskbarCreated) { ui->trayVisible = false; ui->updateTray(); return 0; }
    switch (message) {
    case WM_CREATE: ui->window = hwnd; createControls(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintWindow(hwnd); return 0;
    case WM_SIZE: ui->layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        limits->ptMinTrackSize.x = std::min(ui->s(720), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        limits->ptMinTrackSize.y = std::min(ui->s(440), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        return 0;
    }
    case WM_DPICHANGED: {
        ui->dpi = HIWORD(wp);
        fonts(ui->dpi);
        EnumChildWindows(hwnd, applyFont, 0);
        ui->applyHeadingFonts();
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        const int widths[] = {157, 157, 62, 148, 205, 77, 170};
        for (int i = 0; i < 7; ++i) ListView_SetColumnWidth(ui->list, i, ui->s(widths[i]));
        ui->layout();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!ui->historyMode && ui->settingsPane) {
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            RECT paneRect{}, netRect{}, dnsRect{};
            GetWindowRect(ui->settingsPane, &paneRect);
            GetWindowRect(ui->networks, &netRect);
            GetWindowRect(ui->dns, &dnsRect);
            if (PtInRect(&paneRect, pt) && !PtInRect(&netRect, pt) && !PtInRect(&dnsRect, pt)) {
                ui->scroll -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp), ui->s(60), WHEEL_DELTA);
                ui->layoutSettings();
                return 0;
            }
        }
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: return commonColors(message, wp, lp);
    case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_COMMAND: {
        HWND sender = reinterpret_cast<HWND>(lp);
        if (sender && IsChild(ui->settingsPane, sender) && GetFocus() == sender && isFocusNotification(sender, HIWORD(wp))) {
            RECT item{}, viewport{};
            GetWindowRect(sender, &item);
            GetWindowRect(ui->settingsPane, &viewport);
            const int margin = ui->s(8);
            if (item.top < viewport.top + margin) ui->scroll += item.top - viewport.top - margin;
            else if (item.bottom > viewport.bottom - margin) ui->scroll += std::min(item.bottom - viewport.bottom + margin, item.top - viewport.top - margin);
            ui->layoutSettings();
        }
        if (LOWORD(wp) >= firstDev && LOWORD(wp) <= googleOption && HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == lastDev && SendMessageW(ui->dev[4], BM_GETCHECK, 0, 0) != BST_CHECKED)
                SendMessageW(ui->googleLogin, BM_SETCHECK, BST_UNCHECKED, 0);
            ui->markUnsaved();
            return 0;
        }
        switch (LOWORD(wp)) {
        case navSettings: ui->show(false); return 0;
        case navHistory: ui->show(true); return 0;
        case toggleButton: ui->toggleState(); return 0;
        case saveButton: ui->saveSettings(); return 0;
        case refreshAdaptersButton: ui->loadAdapters(true); ui->updateState(); return 0;
        case exitCommand: DestroyWindow(hwnd); return 0;
        case adapterCombo:
            if (HIWORD(wp) == CBN_SELCHANGE) { ui->fillAdapter(false); ui->markUnsaved(); }
            return 0;
        case networkEdit: case dnsEdit:
            if (HIWORD(wp) == EN_CHANGE && !ui->populating && !ui->engine.active()) ui->markUnsaved();
            return 0;
        case appFilter: case ipFilter:
            if (HIWORD(wp) == EN_CHANGE) ui->refreshHistory(true);
            return 0;
        case clearFilterButton:
            SetWindowTextW(ui->appSearch, L"");
            SetWindowTextW(ui->ipSearch, L"");
            ui->refreshHistory(true);
            return 0;
        case refreshButton: ui->refreshHistory(true); return 0;
        case exportCsvButton: ui->exportHistory(); return 0;
        case copyPathButton: {
            const std::wstring selectedText = text(ui->path);
            if (!selectedText.empty() && copyTextToClipboard(hwnd, selectedText)) {
                ui->historyNotice = L"선택한 경로를 클립보드에 복사했습니다.";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case webCommand: {
            PWSTR programs = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &programs))) {
                ui->failure(L"시작 메뉴 경로를 찾지 못했습니다.");
                return 0;
            }
            const std::wstring linkPath = std::wstring(programs) + L"\\Toggle External Connection\\GitHub Web.lnk";
            CoTaskMemFree(programs);
            if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ui->failure(L"GitHub Web 바로가기를 찾지 못했습니다. 앱을 다시 설치해 확인하세요.");
                return 0;
            }
            wchar_t windows[MAX_PATH]{};
            if (!GetWindowsDirectoryW(windows, MAX_PATH)) {
                ui->failure(L"Windows 폴더를 찾지 못했습니다.");
                return 0;
            }
            const std::wstring linkArg = L"\"" + linkPath + L"\"";
            const auto explorer = std::wstring(windows) + L"\\explorer.exe";
            ShellExecuteW(hwnd, L"open", explorer.c_str(), linkArg.c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDCANCEL: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->hwndFrom == ui->list && hdr->code == LVN_ITEMCHANGED) {
            const int selected = ListView_GetNextItem(ui->list, -1, LVNI_SELECTED);
            const auto selectedRow = selected >= 0 && selected < static_cast<int>(ui->visibleRows.size()) ? &ui->visibleRows[selected] : nullptr;
            if (selectedRow) { ui->selectedKey = *selectedRow; ui->hasSelectedKey = true; }
            else ui->hasSelectedKey = false;
            ui->updateSelectedPathField(selectedRow);
        }
        break;
    }
    case trayMessage:
        if (lp == WM_LBUTTONUP) {
            if (ui->ignoreTrayUpUntil && GetTickCount64() <= ui->ignoreTrayUpUntil) {
                ui->ignoreTrayUpUntil = 0;
            } else {
                ui->ignoreTrayUpUntil = 0;
                SetTimer(hwnd, timerClick, GetDoubleClickTime() + 30, nullptr);
            }
        } else if (lp == WM_LBUTTONDBLCLK) {
            KillTimer(hwnd, timerClick);
            ui->ignoreTrayUpUntil = GetTickCount64() + 1000;
            ui->show(false);
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            KillTimer(hwnd, timerClick);
            ui->menu();
        }
        return 0;
    case WM_TIMER:
        if (wp == timerClick) { KillTimer(hwnd, timerClick); ui->toggleState(); }
        else if (wp == timerRefresh) {
            if (!ui->preview && !ui->trayVisible) ui->updateTray();
            if (ui->healthCheck.valid() && ui->healthCheck.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                const auto result = ui->healthCheck.get();
                if (result.first == ui->generation && ui->engine.active() && !result.second)
                    ui->stop(L"필터 상태를 확인할 수 없어 차단을 종료했습니다. 다시 시작하기 전에 상태를 확인하세요.");
            }
            if (ui->engine.active() && !ui->healthCheck.valid()) {
                const unsigned generation = ui->generation;
                ui->healthCheck = std::async(std::launch::async, [generation] { return std::make_pair(generation, ui->engine.healthy()); });
            }
            if (ui->history) {
                SYSTEMTIME today{};
                GetLocalTime(&today);
                if (ui->day != today.wDay) { ui->day = today.wDay; ui->history->maintain(); }
                const auto error = ui->history->error();
                if (error != ui->historyIssue) { ui->historyIssue = error; ui->addLog(error); ui->updateState(); }
            }
            if (ui->historyMode && IsWindowVisible(hwnd)) ui->refreshHistory(false);
        }
        return 0;
    case WM_CLOSE:
        if (ui->preview) DestroyWindow(hwnd);
        else if (ui->trayVisible) ShowWindow(hwnd, SW_HIDE);
        else if (MessageBoxW(hwnd, L"트레이를 사용할 수 없습니다. 앱을 종료하고 외부 연결을 허용할까요?",
                             L"외부 연결 제어", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        ++ui->generation;
        ui->engine.stop();
        if (ui->healthCheck.valid()) ui->healthCheck.wait();
        if (ui->history) ui->history->flush();
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = hwnd;
        icon.uID = 1;
        if (ui->trayVisible) Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool isTrustedExecutable() {
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, 32768)) return false;
    const std::wstring expected = tec::installedExe();
    if (expected.empty()) return false;
    std::wstring error;
    if (!tec::trustedInstallPath(expected, true, error)) return false;
    HANDLE file = CreateFileW(module, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    wchar_t resolved[32768]{};
    const DWORD length = GetFinalPathNameByHandleW(file, resolved, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!length || length >= 32768) return false;
    std::wstring actual = resolved;
    if (actual.compare(0, 4, L"\\\\?\\") == 0) actual.erase(0, 4);
    return _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}

bool elevate(bool elevatedArgument) {
    if (!isTrustedExecutable()) {
        MessageBoxW(nullptr, L"일반 실행은 보호된 설치 경로에서만 가능합니다.\n설치 프로그램으로 설치한 뒤 실행하세요.\n\nUI만 확인하려면 --preview 또는 --preview-history를 사용하세요.",
                    L"안전한 설치 경로 필요", MB_OK | MB_ICONERROR);
        return false;
    }
    if (IsUserAnAdmin()) return true;
    if (elevatedArgument) {
        MessageBoxW(nullptr, L"관리자 권한을 얻지 못했습니다.", L"실행할 수 없음", MB_OK | MB_ICONERROR);
        return false;
    }
    const std::wstring exe = tec::installedExe();
    const std::wstring workingDirectory = exe.substr(0, exe.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"runas";
    request.lpFile = exe.c_str();
    request.lpDirectory = workingDirectory.c_str();
    request.lpParameters = L"--elevated";
    request.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&request)) {
        const DWORD code = GetLastError();
        MessageBoxW(nullptr, code == ERROR_CANCELLED ? L"관리자 권한 요청이 취소되어 실행을 마쳤습니다."
                                                     : L"관리자 권한으로 시작하지 못했습니다. 설치 상태를 확인하세요.",
                    L"외부 연결 제어", MB_OK | (code == ERROR_CANCELLED ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    return false;
}
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int) {
    assert((split(L" 192.168.1.0/24 \r\n\r\n\tfd12::/64\n ") ==
            std::vector<std::wstring>{L"192.168.1.0/24", L"fd12::/64"}));
    assert(lower(L"C:\\Tools\\APP.EXE").find(L"app.exe") != std::wstring::npos);
    instance = app;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 2 && wcscmp(argv[1], L"--github-web") == 0) { LocalFree(argv); return tec::githubWindow(); }
    Ui state;
    bool elevated = false;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--preview") == 0) state.preview = true;
        else if (wcscmp(argv[i], L"--preview-history") == 0) { state.preview = true; state.historyMode = true; }
        else if (wcscmp(argv[i], L"--elevated") == 0) elevated = true;
        else { LocalFree(argv); MessageBoxW(nullptr, L"지원하지 않는 실행 인수입니다.", L"외부 연결 제어", MB_OK | MB_ICONERROR); return 2; }
    }
    if (argv) LocalFree(argv);
    if (!state.preview && !elevate(elevated)) return 0;
    HANDLE singleton = nullptr;
    if (!state.preview) {
        singleton = CreateMutexW(nullptr, FALSE, L"Global\\ToggleExternalConnection.Production");
        const DWORD mutexError = GetLastError();
        if (!singleton || mutexError == ERROR_ALREADY_EXISTS) {
            if (singleton) CloseHandle(singleton);
            MessageBoxW(nullptr, mutexError == ERROR_ALREADY_EXISTS || mutexError == ERROR_ACCESS_DENIED
                ? L"이 컴퓨터의 다른 창 또는 사용자 세션에서 이미 실행 중입니다.\n실행 중인 세션의 트레이 아이콘을 사용하세요."
                : L"컴퓨터 전체의 실행 상태를 확인하지 못해 시작하지 않았습니다.",
                L"외부 연결 제어", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }
    ui = &state;
    HMODULE user = GetModuleHandleW(L"user32.dll");
    auto perMonitor = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    if (!perMonitor || !perMonitor(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    state.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    fonts(state.dpi);
    backgroundBrush = CreateSolidBrush(background);
    panelBrush = CreateSolidBrush(panel);
    inputBrush = CreateSolidBrush(input);
    editorBrush = CreateSolidBrush(editor);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpfnWndProc = paneProc;
    cls.lpszClassName = L"TecPane";
    RegisterClassExW(&cls);
    cls.lpfnWndProc = windowProc;
    cls.lpszClassName = L"TecWindow";
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(state.s(980), static_cast<int>(work.right - work.left) - state.s(20));
    const int height = std::min(state.s(860), static_cast<int>(work.bottom - work.top) - state.s(20));
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"TecWindow",
        state.preview ? L"외부 연결 제어 — UI 미리 보기" : L"외부 연결 제어",
        WS_OVERLAPPEDWINDOW,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, nullptr);
    if (!window) { if (singleton) CloseHandle(singleton); return 2; }
    if (!state.preview && taskbarCreated) ChangeWindowMessageFilterEx(window, taskbarCreated, MSGFLT_ALLOW, nullptr);
    if (!state.preview) {
        std::wstring error;
        state.dir = tec::storageDir(error);
        if (state.dir.empty()) {
            state.failure(error);
            DestroyWindow(window);
        } else {
            const bool loaded = tec::loadSettings(state.dir, state.saved, error);
            if (!error.empty()) state.addLog(error);
            state.history.reset(new tec::History(state.dir));
            const auto historyError = state.history->error();
            state.historyIssue = historyError;
            if (!historyError.empty()) state.addLog(historyError);
            state.loadAdapters(false);
            state.confirmed = loaded && state.adapterIndex() >= 0;
            state.unsaved = false;
            for (unsigned i = 0; i < 5; ++i)
                SendMessageW(state.dev[i], BM_SETCHECK, (state.saved.apps & (1u << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state.googleLogin, BM_SETCHECK, (state.saved.apps & 16u) && tec::googleLoginAllowed() ? BST_CHECKED : BST_UNCHECKED, 0);
            state.updateAppStatus();
            if (state.confirmed) state.addLog(L"저장된 인트라넷 설정을 사용합니다.");
        }
    } else {
        state.loadAdapters(false);
        state.updateAppStatus();
        state.addLog(L"읽기 전용 UI 미리 보기입니다. 표시된 어댑터와 기록은 예시입니다.");
    }
    if (IsWindow(window)) {
        state.updateState();
        state.show(state.historyMode);
        SetTimer(window, timerRefresh, 2000, nullptr);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            HWND focused = GetFocus();
            if (focused == state.networks || focused == state.dns || focused == state.appSearch ||
                focused == state.ipSearch || focused == state.path) {
                SendMessageW(focused, EM_SETSEL, 0, -1);
                continue;
            }
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    state.engine.stop();
    if (state.history) state.history->flush();
    state.history.reset();
    if (singleton) CloseHandle(singleton);
    DeleteObject(normalFont);
    DeleteObject(headingFont);
    DeleteObject(titleFont);
    DeleteObject(backgroundBrush);
    DeleteObject(panelBrush);
    DeleteObject(inputBrush);
    DeleteObject(editorBrush);
    return 0;
}
