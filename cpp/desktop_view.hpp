#pragma once

#include "ui_text.hpp"
#include "audio_player.hpp"
#include "numeric_edit.hpp"
#include "resource.h"
#include "phone_setup_runtime.hpp"
#include "user_startup.hpp"
#include "update_service.hpp"
#include "update_installer.hpp"
#include "local_ipv4.hpp"
#include "speech_ui.hpp"
#include "tray_identity.hpp"

/// Load a shared embedded product icon at the system's large or small native size.
static HICON applicationIcon(bool smallSize = false) {
    const int width = GetSystemMetrics(smallSize ? SM_CXSMICON : SM_CXICON);
    const int height = GetSystemMetrics(smallSize ? SM_CYSMICON : SM_CYICON);
    HICON icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_PHONERECORD),
        IMAGE_ICON, width, height, LR_SHARED));
    if (!icon) { LOGW("APP_ICON_LOAD_FAILED size=%dx%d win32=%lu", width, height, GetLastError()); return LoadIconW(NULL, IDI_APPLICATION); }
    LOGD("APP_ICON_LOADED resource=%d size=%dx%d", IDI_PHONERECORD, width, height);
    return icon;
}

/// Native window handle used only for asynchronous media notifications.
static std::atomic<HWND> g_mainWindow{NULL};
/// Count of files currently being written, distinct from allocated control sessions.
static std::atomic<int> g_activeRecordings{0};
/// Native callback message emitted on the first successfully written RTP packet.
static const UINT kMediaNotification = WM_APP + 21;
/// Native callback message emitted after a WAV has closed.
static const UINT kFileNotification = WM_APP + 22;
/// Explicit offline-only render destination for native visual regression captures.
static wstring g_uiCaptureDirectory;

/// Notify the GUI without passing caller details through a cross-thread text buffer.
static void desktopRecordingStarted(const string&, const string&) {
    ++g_activeRecordings;
    if (HWND window = g_mainWindow.load()) PostMessageW(window, kMediaNotification, 0, 0);
}

/// Refresh the completed recording list on the GUI thread.
static void desktopRecordingStopped() {
    if (g_activeRecordings > 0) --g_activeRecordings;
    if (HWND window = g_mainWindow.load()) PostMessageW(window, kFileNotification, 0, 0);
}

/// Product native desktop view; every control remains on the owning GUI thread.
class DesktopView {
#ifdef PHONERECORD_NETWORK_TESTING
    /// Exercise draft preservation and completion ordering without reading real adapters.
    friend struct LocalIpv4Fixture;
#endif
#ifdef PHONERECORD_UPDATE_TESTING
    /// Observe the real owned tray controls in the separate native regression executable.
    friend struct UpdateTrayFixture;
#endif
public:
    /// Adopt a system-DPI scale; native resources are created in WM_CREATE.
    explicit DesktopView(int dpi, HANDLE stopSignal = NULL) : dpi_(dpi), stopSignal_(stopSignal) {}
    /// Release every owned GDI object after the native window has been destroyed.
    ~DesktopView() { updates_.stop(); if (localIpWorker_.joinable()) localIpWorker_.join(); if (updateIcon_) DestroyIcon(updateIcon_); destroyFonts(); if (background_) DeleteObject(background_); if (white_) DeleteObject(white_); }

    /// Hide a login-started window only when its tray entry can restore it.
    bool hasTrayIcon() const { return trayAdded_; }

    /// Route native messages to the instance supplied to CreateWindowExW.
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        DesktopView* self = reinterpret_cast<DesktopView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<DesktopView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

private:
    /// Convert the view's logical pixels to current device pixels.
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    /// Return current logical client width.
    int width() const { RECT area{}; GetClientRect(window_, &area); return MulDiv(area.right, 96, dpi_); }
    /// Return current logical client height.
    int height() const { RECT area{}; GetClientRect(window_, &area); return MulDiv(area.bottom, 96, dpi_); }

    /// One control belongs to one page, or page -1 for the persistent sidebar.
    struct Control {
        /// Native child window.
        HWND window;
        /// Page index (0 overview, 1 recordings, 2 diagnostics, 3 settings).
        int page;
        /// Stable command identifier.
        int id;
        /// Resource key for immediate language changes; edits and user data leave this empty.
        string textKey;
    };

    /// Create one native child and register ownership/page visibility.
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, int page, DWORD extended = 0) {
        HWND child = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), NULL, NULL);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        controls_.push_back({child, page, id, ""});
        if (page >= 0 && page != page_) ShowWindow(child, SW_HIDE);
        return child;
    }

    /// Create a styled owner-drawn action button using the central resource key.
    HWND button(const char* key, int id, int page) { return localizedControl(L"BUTTON", key, WS_TABSTOP | BS_OWNERDRAW, id, page); }

    /// Register static product copy for in-place retranslation without recreating controls.
    HWND localizedControl(const wchar_t* type, const char* key, DWORD style, int id, int page, DWORD extended = 0) {
        HWND child = control(type, uiText(key), style, id, page, extended); controls_.back().textKey = key; return child;
    }

    /// Create a single-line text input with a clear keyboard focus border.
    HWND edit(const string& text, int id, int page, bool password = false) {
        HWND child = control(L"EDIT", utf8ToWide(text).c_str(), WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0), id, page, WS_EX_CLIENTEDGE);
        SendMessageW(child, EM_SETLIMITTEXT, password ? 256 : 1024, 0);
        return child;
    }

    /// Create a modern native report list with double buffering and full row selection.
    HWND list(int id, int page, const std::vector<std::pair<const char*, int>>& columns) {
        HWND child = control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, id, page, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(child, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(child, L"Explorer", NULL);
        int index = 0;
        for (const auto& column : columns) { LVCOLUMNW value{}; value.mask = LVCF_TEXT | LVCF_WIDTH; value.pszText = const_cast<wchar_t*>(uiText(column.first)); value.cx = px(column.second); ListView_InsertColumn(child, index++, &value); columnKeys_[child].push_back(column.first); }
        return child;
    }

    /// Read the current edit value using Unicode, including paths with non-ASCII characters.
    string text(HWND editWindow) const {
        const int length = GetWindowTextLengthW(editWindow);
        wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(editWindow, &value[0], length + 1); value.resize(length);
        return wideToUtf8(value);
    }

    /// Position an owned native child in logical pixels.
    void move(HWND child, int x, int y, int w, int h) { MoveWindow(child, px(x), px(y), px(std::max(w, 1)), px(std::max(h, 1)), TRUE); }

    /// Create the font hierarchy at the actual system/monitor DPI.
    void createFonts() {
        NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
        const bool systemFont = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
        const wchar_t* face = systemFont ? metrics.lfMessageFont.lfFaceName : L"Segoe UI";
        font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        smallFont_ = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        titleFont_ = CreateFontW(-px(26), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        valueFont_ = CreateFontW(-px(19), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    }

    /// Delete owned fonts once no control will paint with them.
    void destroyFonts() { for (HFONT value : {font_, smallFont_, titleFont_, valueFont_}) if (value) DeleteObject(value); }

    /// Build all pages; page switching changes visibility without recreating controls.
    void create() {
        g_mainWindow = window_;
        createFonts(); background_ = CreateSolidBrush(RGB(246, 248, 252)); white_ = CreateSolidBrush(RGB(255, 255, 255));
        button("nav.overview", 100, -1); button("nav.recordings", 101, -1); button("nav.diagnostics", 102, -1); button("nav.settings", 103, -1); button("nav.help", 104, -1);
#ifdef _WIN64
        button("speech.module_live", 107, -1);
        button("speech.nav_history",109,-1);
#endif
        button("nav.updates", 105, -1);
        button("button.exit", 902, -1);
        languageChoice_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 430, -1);
        for (const wchar_t* language : {L"简体中文", L"繁體中文", L"English"}) SendMessageW(languageChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(language));
        SendMessageW(languageChoice_, CB_SETCURSEL, g_uiLanguage.load(), 0);
        // Project link beside the version. The visible control is the GitHub mark only.
        control(L"BUTTON", L"GitHub", WS_TABSTOP | BS_OWNERDRAW, 106, -1);
        button("button.compatibility", 502, 0); button("button.more", 506, 0);
        followRecent_ = localizedControl(L"BUTTON", "button.follow", WS_TABSTOP | BS_AUTOCHECKBOX, 505, 0);
        SendMessageW(followRecent_, BM_SETCHECK, BST_CHECKED, 0);
        recent_ = list(600, 0, {{"column.time", 170}, {"column.level", 65}, {"column.event", 700}});
        recordingSearch_ = edit("", 210, 1);
        SendMessageW(recordingSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("search.recordings")));
        button("button.play", 211, 1); button("button.refresh", 214, 1);
        localizedControl(L"STATIC", "recordings.context_hint", SS_LEFT | SS_ENDELLIPSIS, 632, 1);
        recordings_ = list(610, 1, {{"column.time", 150}, {"column.title", 160}, {"column.direction", 58}, {"column.contact", 170}, {"column.duration", 64}, {"column.quality", 125}, {"column.source", 120}, {"column.size", 76}});
        playerSeek_ = control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 218, 1);
        SendMessageW(playerSeek_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        button("player.pause", 216, 1); button("player.stop", 217, 1);
        playerCaption_ = control(L"STATIC", uiText("player.empty"), SS_LEFT | SS_ENDELLIPSIS, 219, 1);
        playerTime_ = control(L"STATIC", L"0:00 / 0:00", SS_RIGHT, 220, 1);
        logSearch_ = edit("", 310, 2);
        SendMessageW(logSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("search.logs")));
        SendMessageW(fields_[4], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("settings.local_ip_hint")));
        logFilter_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 311, 2);
        SendMessageW(logFilter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText("filter.all")));
        SendMessageW(logFilter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText("filter.problems")));
        SendMessageW(logFilter_, CB_SETCURSEL, 0, 0);
        button("button.copy", 312, 2); button("button.more", 315, 2);
        diagnostics_ = list(620, 2, {{"column.sequence", 55}, {"column.time", 170}, {"column.level", 65}, {"column.event", 850}});
        detail_ = control(L"EDIT", uiText("diagnostics.placeholder"), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 621, 2, WS_EX_CLIENTEDGE);
        const string values[] = {g_cfg.phoneIp, std::to_string(g_cfg.phonePort), g_cfg.phoneUser, g_cfg.phonePass, g_cfg.advertiseIp, std::to_string(g_cfg.controlPort), std::to_string(g_cfg.hookPort == 0 ? 18080 : g_cfg.hookPort), std::to_string(g_cfg.udpPortStart), std::to_string(g_cfg.udpPortEnd), std::to_string(g_cfg.idleTimeoutSec), g_cfg.recordDir};
        const char* labels[] = {"field.phone_ip", "field.phone_port", "field.user", "field.password", "field.local_ip", "field.control", "field.event", "field.udp_start", "field.udp_end", "field.timeout", "field.directory"};
        for (int index = 0; index < 11; ++index) {
            fieldLabels_[index] = localizedControl(L"STATIC", labels[index], SS_LEFT, 700 + index, 3);
            fields_[index] = edit(values[index], 400 + index, 3, index == 3);
        }
        SendMessageW(fields_[4], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("settings.local_ip_hint")));
        requestLocalIpSuggestion();
        addressModeLabel_ = localizedControl(L"STATIC", "field.phone_address_mode", SS_LEFT, 726, 3);
        addressModeChoice_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 416, 3);
        for (const char* key : {"address.fixed", "address.dhcp"}) SendMessageW(addressModeChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(key)));
        SendMessageW(addressModeChoice_, CB_SETCURSEL, g_cfg.phoneAddressMode == "dhcp" ? 1 : 0, 0);
        addressStatus_ = control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 728, 3);
        addressIdentityLabel_ = localizedControl(L"STATIC", "field.phone_mac", SS_LEFT, 727, 3);
        addressMac_ = edit(g_cfg.phoneMac, 417, 3);
        for (int index : {1, 5, 6, 7, 8}) NumericEdit::attach(fields_[index], 65535);
        NumericEdit::attach(fields_[9], 120);
        externalChoice_ = localizedControl(L"BUTTON", "field.external", WS_TABSTOP | BS_AUTOCHECKBOX, 413, 3);
        SendMessageW(externalChoice_, BM_SETCHECK, g_cfg.hookPort == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(fields_[6], g_cfg.hookPort != 0);
        closeLabel_ = localizedControl(L"STATIC", "field.close", SS_LEFT, 723, 3);
        closeChoice_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 411, 3);
        for (const char* key : {"close.tray", "close.minimize", "close.exit"}) SendMessageW(closeChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(key)));
        SendMessageW(closeChoice_, CB_SETCURSEL, g_closeAction.load(), 0);
        intervalLabel_ = localizedControl(L"STATIC", "field.health", SS_LEFT, 724, 3);
        intervalChoice_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 412, 3);
        const char* intervalKeys[] = {"interval.off", "interval.30", "interval.60", "interval.120", "interval.300"};
        int selectedInterval = 2;
        for (int index = 0; index < 5; ++index) { SendMessageW(intervalChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(intervalKeys[index]))); if (intervals_[index] == g_healthIntervalSec) selectedInterval = index; }
        // Preserve a valid hand-edited custom interval instead of silently replacing it.
        if (std::find(std::begin(intervals_), std::end(intervals_), g_cfg.healthIntervalSec) == std::end(intervals_)) {
            const wstring custom = std::to_wstring(g_cfg.healthIntervalSec) + uiText("interval.seconds");
            SendMessageW(intervalChoice_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(custom.c_str())); selectedInterval = 5;
        }
        SendMessageW(intervalChoice_, CB_SETCURSEL, selectedInterval, 0);
        notificationChoice_ = localizedControl(L"BUTTON", "field.notifications", WS_TABSTOP | BS_AUTOCHECKBOX, 414, 3);
        SendMessageW(notificationChoice_, BM_SETCHECK, g_cfg.trayNotifications ? BST_CHECKED : BST_UNCHECKED, 0);
        startupChoice_ = localizedControl(L"BUTTON", "field.startup", WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY, 415, 3);
        refreshStartup();
        localizedControl(L"STATIC", "field.record_folder_mode", SS_LEFT, 731, 3);
        storageMode_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 418, 3);
        const char* storageModes[] = {"flat", "daily", "weekly", "custom"};
        for (int index = 0; index < 4; ++index) {
            SendMessageW(storageMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText((string("storage.") + storageModes[index]).c_str())));
            if (g_cfg.recordFolderMode == storageModes[index]) SendMessageW(storageMode_, CB_SETCURSEL, index, 0);
        }
        localizedControl(L"STATIC", "field.record_folder_pattern", SS_LEFT, 732, 3);
        storagePattern_ = edit(g_cfg.recordFolderPattern, 419, 3);
        SendMessageW(storagePattern_, EM_SETLIMITTEXT, 120, 0);
        storagePreview_ = control(L"STATIC", L"", SS_LEFT, 733, 3);
        button("button.save", 420, 3); button("button.browse", 421, 3); button("button.more", 435, 3);
        button("button.phone_setup_direct", 424, 3);
        button("settings.basic", 432, 3); button("settings.advanced", 433, 3); button("settings.preferences", 434, 3);
        basicIpSummary_ = control(L"STATIC", L"", SS_LEFT, 729, 3);
        localizedControl(L"STATIC", "settings.basic_hint", SS_LEFT, 730, 3);
        settingNote_ = localizedControl(L"EDIT", "settings.note", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 722, 3, WS_EX_CLIENTEDGE);
        SendMessageW(settingNote_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        helpTopic_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 800, 4);
        for (const char* key : {"help.topic.start", "help.topic.fields", "help.topic.ports", "help.topic.devices", "help.topic.diagnostics", "help.topic.compatibility", "help.topic.setup"}) SendMessageW(helpTopic_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(key)));
        SendMessageW(helpTopic_, CB_SETCURSEL, 0, 0);
        button("button.compatibility", 804, 4); button("button.compatibility_copy", 805, 4);
        helpText_ = control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 801, 4, WS_EX_CLIENTEDGE);
        refreshHelp(); refreshAddressMode(); showFieldHelp(-1);
        button("update.check", 820, 5); button("update.download", 821, 5); button("update.install", 822, 5);
        button("update.preferences", 830, 5);
        localizedControl(L"STATIC", "update.interval", SS_LEFT, 828, 5);
        updateInterval_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 824, 5);
        for (const char* key : {"update.off", "update.hour1", "update.hour6", "update.hour12", "update.hour24"}) SendMessageW(updateInterval_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(key)));
        updateBalloons_ = localizedControl(L"BUTTON", "update.balloons", WS_TABSTOP | BS_AUTOCHECKBOX, 825, 5);
        updateStatus_ = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 826, 5, WS_EX_CLIENTEDGE);
        updateNotes_ = control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 827, 5, WS_EX_CLIENTEDGE);
        localizedControl(L"STATIC", "update.explanation", SS_LEFT, 829, 5);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1; tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_.uCallbackMessage = WM_APP + 31; tray_.hIcon = applicationIcon(true); wcscpy_s(tray_.szTip, L"PhoneRecord 2");
        if (!initializeTrayIdentity(tray_)) LOGW("TRAY_STABLE_IDENTITY_FAILED win32=%lu", GetLastError());
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        updates_.start(window_, g_configPath, g_offline);
        const auto preferences = updates_.preferences();
        const int choices[] = {0, 1, 6, 12, 24};
        for (int index = 0; index < 5; ++index) if (choices[index] == preferences.hours) SendMessageW(updateInterval_, CB_SETCURSEL, index, 0);
        SendMessageW(updateBalloons_, BM_SETCHECK, preferences.balloons ? BST_CHECKED : BST_UNCHECKED, 0);
        refreshUpdates(true);
        SetTimer(window_, 1, 200, NULL); layout(); updatePlayer(); refreshLogs(true); refreshRecordings(true);
    }

    /// Logical width of the "v<version>" label, used to place the project mark after it.
    int versionLabelWidth(HDC dc) const {
        if (!dc || !smallFont_) return 48;
        const wstring version = L"v" + utf8ToWide(kProductVersion);
        SIZE extent{};
        HGDIOBJ old = SelectObject(dc, smallFont_);
        GetTextExtentPoint32W(dc, version.c_str(), static_cast<int>(version.size()), &extent);
        SelectObject(dc, old);
        return std::max(1, MulDiv(extent.cx, 96, dpi_));
    }

    /// Lay out all pages without fixed-resolution assumptions.
    void layout() {
        const int w = width(), h = height(), left = 200, available = w - left - 24;
        for (const auto& child : controls_) {
            if (child.id >= 100 && child.id <= 104) {
                int row = child.id - 100;
#ifdef _WIN64
                if (child.id >= 102) row += 2;
#endif
                #ifdef _WIN64
                move(child.window,14,118+row*46,148,40);
#else
                move(child.window,14,118+row*54,148,42);
#endif
            }
#ifdef _WIN64
            if (child.id == 107) move(child.window,14,210,148,40);
            if (child.id == 109) move(child.window,14,256,148,40);
#endif
            if (child.id == 902) move(child.window, 14, h - 88, 148, 34);
            if (child.id == 502) move(child.window, left, 460, 240, 34);
            if (child.id == 506) move(child.window, left + 252, 460, 148, 34);
            if (child.id == 505) move(child.window, w - 182, 464, 158, 28);
        }
        move(languageChoice_, 14, h - 139, 148, 28);
        move(GetDlgItem(window_, 105), 14, h - 211, 148, 34);
        move(GetDlgItem(window_, 828), left, 110, 190, 24); move(updateInterval_, left + 200, 106, 180, 200);
        move(updateBalloons_, left + 400, 106, available - 400, 30);
        move(updateStatus_, left, 152, available, 98);
        move(GetDlgItem(window_, 820), left, 264, 160, 34); move(GetDlgItem(window_, 821), left + 174, 264, 220, 34); move(GetDlgItem(window_, 822), left + 174, 264, 220, 34);
        move(GetDlgItem(window_, 830), w - 224, 106, 200, 34);
        move(GetDlgItem(window_, 829), left, 312, available, 88); move(updateNotes_, left, 414, available, h - 452);
        if (HWND link = GetDlgItem(window_, 106)) {
            HDC dc = GetDC(window_);
            const int textWidth = versionLabelWidth(dc);
            if (dc) ReleaseDC(window_, dc);
            const int hit = 22;
            move(link, std::min(21 + textWidth + 5, 176 - 8 - hit), h - 42, hit, hit);
        }
        const bool reportTopic = SendMessageW(helpTopic_, CB_GETCURSEL, 0, 0) == 5;
        move(helpTopic_, left, 110, reportTopic ? available - 430 : available, 240); move(GetDlgItem(window_, 804), w - 442, 110, 246, 32); move(GetDlgItem(window_, 805), w - 184, 110, 160, 32); move(helpText_, left, 158, available, h - 200);
        move(recent_, left, 534, available, h - 576);
        move(recordingSearch_, left, 110, available, 32);
        const int recordingActions[] = {211, 214};
        const int recordingWidths[] = {88, 98};
        int actionLeft = left;
        for (size_t index = 0; index < std::size(recordingActions); ++index) { move(GetDlgItem(window_, recordingActions[index]), actionLeft, 152, recordingWidths[index], 32); actionLeft += recordingWidths[index] + 10; }
        move(GetDlgItem(window_, 632), actionLeft + 8, 156, available - (actionLeft - left) - 8, 26);
        move(recordings_, left, 198, available, h - 380);
        move(playerCaption_, left + 16, h - 164, available - 32, 24);
        move(playerSeek_, left + 10, h - 128, available - 178, 28);
        move(playerTime_, w - 178, h - 128, 136, 26);
        move(GetDlgItem(window_, 216), left + 16, h - 86, 112, 32);
        move(GetDlgItem(window_, 217), left + 140, h - 86, 112, 32);
        move(logSearch_, left, 110, available - 432, 32);
        move(logFilter_, w - 444, 110, 136, 240);
        move(GetDlgItem(window_, 312), w - 296, 110, 116, 32);
        move(GetDlgItem(window_, 315), w - 168, 110, 144, 32);
        move(diagnostics_, left, 158, available, h - 354);
        move(detail_, left, h - 182, available, 134);
        const int column = available / 2;
        for (int index = 0; index < 3; ++index) move(GetDlgItem(window_, 432 + index), left + index * 176, 110, 164, 34);
        const auto settingField = [&](int index, bool right, int y, int fieldWidth = 0) {
            const int x = left + (right ? column + 10 : 0), size = fieldWidth ? fieldWidth : column - 24;
            move(fieldLabels_[index], x, y, size, 22); move(fields_[index], x, y + 25, size, 32);
        };
        settingField(0, false, 176); settingField(2, false, 252); settingField(3, true, 252);
        move(fieldLabels_[10], left, 330, available, 22); move(fields_[10], left, 355, available - 120, 32);
        move(GetDlgItem(window_, 421), w - 136, 355, 112, 32);
        move(addressStatus_, left + column + 10, settingsGroup_ == 0 ? 176 : 148, column - 24, 26);
        move(basicIpSummary_, left + column + 10, 210, column - 24, 36);
        move(GetDlgItem(window_, 730), left, 412, available, 58);
        move(addressModeLabel_, left, 166, column - 24, 22); move(addressModeChoice_, left, 191, column - 24, 220);
        move(addressIdentityLabel_, left + column + 10, 166, column - 24, 22); move(addressMac_, left + column + 10, 191, column - 24, 32);
        settingField(1, false, 244); settingField(4, true, 244);
        settingField(5, false, 322); settingField(6, true, 322, 110);
        move(externalChoice_, left + column + 130, 347, column - 144, 32);
        settingField(7, false, 400); settingField(8, true, 400); settingField(9, false, 478);
        move(intervalLabel_, left + column + 10, 478, column - 24, 22); move(intervalChoice_, left + column + 10, 503, column - 24, 200);
        move(closeLabel_, left, 176, available, 22); move(closeChoice_, left, 201, std::min(available, 460), 180);
        move(notificationChoice_, left, 260, available, 30); move(startupChoice_, left, 310, available, 30);
        move(GetDlgItem(window_, 731), left, 376, column - 24, 22); move(storageMode_, left, 401, column - 24, 200);
        move(GetDlgItem(window_, 732), left + column + 10, 376, column - 24, 22); move(storagePattern_, left + column + 10, 401, column - 24, 32);
        move(storagePreview_, left, 449, available, 64);
        const int noteTop = settingsGroup_ == 0 ? 482 : settingsGroup_ == 1 ? 554 : 530;
        move(settingNote_, left, noteTop, available, std::max(40, std::min(settingsGroup_ == 1 ? 68 : 110, h - 96 - noteTop)));
        move(GetDlgItem(window_, 420), left, h - 74, 136, 36);
        move(GetDlgItem(window_, 424), left + 148, h - 74, 180, 36);
        move(GetDlgItem(window_, 435), w - 188, h - 74, 164, 36);
        refreshSettingsVisibility();
        refreshActionVisibility();
        if (recordings_) {
            const int titles = std::max(320, available - 593);
            ListView_SetColumnWidth(recordings_, 1, px(titles / 2));
            ListView_SetColumnWidth(recordings_, 3, px(titles - titles / 2));
        }
        if (diagnostics_) ListView_SetColumnWidth(diagnostics_, 3, px(std::max(200, available - 315)));
        if (recent_) ListView_SetColumnWidth(recent_, 2, px(std::max(200, available - 265)));
#ifdef _WIN64
        SpeechPage::place(left, 110, available, h - 134, dpi_);
        if (SpeechPage::instance()) ShowWindow(SpeechPage::instance(), (page_ == 6 || page_ == 8) ? SW_SHOW : SW_HIDE);
#endif
        InvalidateRect(window_, NULL, FALSE);
    }

    /// Keep every settings draft alive while exposing only its selected category.
    void refreshSettingsVisibility() {
        if (!basicIpSummary_) return;
        for (const auto& child : controls_) if (child.page == 3) {
            const int id = child.id;
            int group = 1;
            if (id == 400 || id == 402 || id == 403 || id == 410 || id == 700 || id == 702 || id == 703 || id == 710 || id == 421 || id == 729 || id == 730) group = 0;
            else if (id == 411 || id == 723 || id == 414 || id == 415 || id == 418 || id == 419 || (id >= 731 && id <= 733)) group = 2;
            else if (id == 722 || id == 420 || id == 435 || (id >= 432 && id <= 434)) group = -1;
            else if (id == 424) group = 0;
            bool visible = page_ == 3 && (group < 0 || group == settingsGroup_);
            if (id == 728) visible = page_ == 3 && settingsGroup_ == 0;
            if (id == 419 || id == 732) visible = visible && storageModeDraft() == "custom";
            if (id == 417 || id == 727) visible = visible && addressModeDraft() == "dhcp";
            ShowWindow(child.window, visible ? SW_SHOW : SW_HIDE);
        }
        const string address = trim(text(fields_[4]));
        const wstring summary = wstring(uiText("settings.local_summary")) + L"\r\n" + (address.empty() ? uiText("settings.local_pending") : utf8ToWide(address));
        SetWindowTextW(basicIpSummary_, summary.c_str());
        refreshStoragePreview();
    }

    /// Read a stable storage policy independently from localized labels.
    string storageModeDraft() const {
        const int index = static_cast<int>(SendMessageW(storageMode_, CB_GETCURSEL, 0, 0));
        const char* modes[] = {"flat", "daily", "weekly", "custom"};
        return index >= 0 && index < 4 ? modes[index] : "flat";
    }

    /// Preview the destination without creating folders or modifying saved/runtime settings.
    void refreshStoragePreview() {
        if (!storagePreview_) return;
        string folder;
        const bool valid = recordingSubfolder(storageModeDraft(), text(storagePattern_), recordingLocalTime(), folder);
        const wstring value = valid ? wstring(uiText("storage.preview")) + utf8ToWide(trim(text(fields_[10])) + (folder.empty() ? "" : "\\" + folder)) : uiText("storage.invalid");
        SetWindowTextW(storagePreview_, value.c_str());
    }

    /// Expose report and update actions only when their current state makes them useful.
    void refreshActionVisibility() {
        const bool report = page_ == 4 && SendMessageW(helpTopic_, CB_GETCURSEL, 0, 0) == 5;
        for (int id : {804, 805}) ShowWindow(GetDlgItem(window_, id), report ? SW_SHOW : SW_HIDE);
        // The existing controls retain preference state; the native menu is their visible entry.
        for (int id : {824, 825, 828}) ShowWindow(GetDlgItem(window_, id), SW_HIDE);
        const auto update = updates_.state();
        ShowWindow(GetDlgItem(window_, 821), page_ == 5 && !update.manifest.version.empty() && update.staged.empty() ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window_, 822), page_ == 5 && !update.staged.empty() ? SW_SHOW : SW_HIDE);
    }

    /// Open page-specific secondary actions from a keyboard-accessible More button.
    void moreActions(int anchor) {
        HMENU menu = CreatePopupMenu(); if (!menu) return;
        const auto item = [&](int id, const char* key, bool enabled = true) {
            AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, uiText(key));
        };
        if (anchor == 315) { item(313, "button.export"); item(314, "button.logs"); }
        else {
            if (anchor == 435) item(423, "button.compatibility", IsWindowEnabled(GetDlgItem(window_, 502)) != FALSE);
            item(504, "button.phone_web", !phoneAddress().empty());
            item(422, "button.setup_guide");
        }
        RECT bounds{}; GetWindowRect(GetDlgItem(window_, anchor), &bounds);
        const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, bounds.left, bounds.bottom, 0, window_, NULL);
        DestroyMenu(menu);
        if (action) command(static_cast<int>(action));
    }

    /// Edit update scheduling and notifications without saving any connection draft.
    void updatePreferencesMenu() {
        HMENU menu = CreatePopupMenu(); if (!menu) return;
        const auto preferences = updates_.preferences();
        const int hours[] = {0, 1, 6, 12, 24};
        const char* keys[] = {"update.off", "update.hour1", "update.hour6", "update.hour12", "update.hour24"};
        for (int index = 0; index < 5; ++index)
            AppendMenuW(menu, MF_STRING | (preferences.hours == hours[index] ? MF_CHECKED : 0), 840 + index, uiText(keys[index]));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (preferences.balloons ? MF_CHECKED : 0), 845, uiText("update.balloons"));
        RECT bounds{}; GetWindowRect(GetDlgItem(window_, 830), &bounds);
        const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, bounds.left, bounds.bottom, 0, window_, NULL);
        DestroyMenu(menu);
        if (action) command(static_cast<int>(action));
    }

    /// Change only the visible category; this never saves, reloads or resets configuration.
    void selectSettingsGroup(int group) {
        if (group < 0 || group > 2) return;
        settingsGroup_ = group; showFieldHelp(-1); layout();
        for (int id = 432; id <= 434; ++id) InvalidateRect(GetDlgItem(window_, id), NULL, TRUE);
    }

    /// Switch pages, keeping their visible controls and their diagnostic snapshots consistent.
    void switchPage(int page) {
        page_ = page;
        if (page == 3) refreshStartup();
        for (const auto& child : controls_) if (child.page >= 0) ShowWindow(child.window, child.page == page ? SW_SHOW : SW_HIDE);
        if (page == 1) refreshRecordings(true);
        if (page == 2) refreshLogs(true);
        if (page == 4) refreshHelp();
        refreshAddressMode();
        layout();
        // Owner-drawn children do not inherit their parent's invalidation. Explicitly
        // repaint all menu items so precisely one item reflects the current page.
        for (int id = 100; id <= 105; ++id) RedrawWindow(GetDlgItem(window_, id), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
#ifdef _WIN64
        RedrawWindow(GetDlgItem(window_, 107), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        RedrawWindow(GetDlgItem(window_,109),NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
#endif
    }

    /// Refill a translated combo while preserving its semantic selected index.
    void translateCombo(HWND choice, const std::vector<const char*>& keys, bool customInterval = false) {
        const LRESULT selected = SendMessageW(choice, CB_GETCURSEL, 0, 0);
        SendMessageW(choice, CB_RESETCONTENT, 0, 0);
        for (const char* key : keys) SendMessageW(choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText(key)));
        if (customInterval && selected >= static_cast<LRESULT>(keys.size())) {
            const wstring value = std::to_wstring(g_cfg.healthIntervalSec) + uiText("interval.seconds");
            SendMessageW(choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        SendMessageW(choice, CB_SETCURSEL, std::max<LRESULT>(0, selected), 0);
    }

    /// Resolve a validation field to translated product copy while retaining its stable config key.
    wstring fieldDisplayName(const string& field) const {
        const std::pair<const char*, const char*> fields[] = {
            {"phone_address_mode", "field.phone_address_mode"}, {"phone_mac", "field.phone_mac"}, {"phone_ip", "field.phone_ip"}, {"phone_port", "field.phone_port"}, {"phone_user", "field.user"},
            {"phone_pass_dpapi", "field.password"}, {"advertise_ip", "field.local_ip"}, {"control_port", "field.control"},
            {"hook_port", "field.event"}, {"udp_port_range", "field.udp_start"}, {"idle_timeout", "field.timeout"},
            {"record_folder_mode", "field.record_folder_mode"}, {"record_folder_pattern", "field.record_folder_pattern"}, {"health_interval", "field.health"}, {"close_action", "field.close"}, {"record_dir", "field.directory"}, {"tray_notifications", "field.notifications"}
        };
        for (const auto& value : fields) if (field == value.first) return uiText(value.second);
        return utf8ToWide(field);
    }

    /// Show the live target independently from unsaved settings and the immutable discovery hint.
    void refreshAddressStatus() {
        const string address = phoneAddress();
        const wstring value = wstring(uiText("address.current")) + utf8ToWide(address.empty() ? "—" : address);
        SetWindowTextW(addressStatus_, value.c_str());
        lastAddressRevision_ = g_phoneAddressRevision;
    }

    /// Read the semantic mode without translating or using display labels as configuration values.
    string addressModeDraft() const {
        const LRESULT index = SendMessageW(addressModeChoice_, CB_GETCURSEL, 0, 0);
        return index == 1 ? "dhcp" : "fixed";
    }

    /// Retain inactive drafts while showing the mode's identity source and editable fields.
    void refreshAddressMode() {
        refreshAddressStatus();
        const bool dhcp = addressModeDraft() == "dhcp";
        SetWindowTextW(addressIdentityLabel_, uiText("field.phone_mac"));
        refreshSettingsVisibility();
        EnableWindow(addressMac_, dhcp); EnableWindow(fields_[0], TRUE);
        if (dhcp) { SendMessageW(externalChoice_, BM_SETCHECK, BST_UNCHECKED, 0); EnableWindow(fields_[6], TRUE); }
        EnableWindow(externalChoice_, !dhcp);
        if (settingsGroup_ == 1) SetWindowTextW(settingNote_, uiText(dhcp ? "address.help_dhcp" : "address.help_fixed"));
        else showFieldHelp(-1);
    }

    /// Show the selected setting's purpose in a scrollable inline note, without touching its value.
    void showFieldHelp(int id) {
        focusedSetting_ = id;
        const char* keys[] = {"phone_ip", "phone_port", "user", "password", "local_ip", "control", "event", "udp_start", "udp_end", "timeout", "directory", "record_folder_mode", "record_folder_pattern", "close", "health", "external", "notifications", "startup"};
        if (id == 418 || id == 419) { SetWindowTextW(settingNote_, uiText("storage.help"));
        } else if (id >= 416 && id <= 417) {
            SetWindowTextW(settingNote_, uiText(id == 417 ? "help.field.phone_mac" : "help.field.phone_address_mode"));
        } else if (id == 415) {
            const auto state = currentStartup().read();
            const char* status = state.error ? "startup.unavailable" : state.enabled ? state.thisCopy ? "startup.enabled" : "startup.other_copy" : "startup.disabled";
            const wstring message = wstring(uiText(status)) + L"\r\n" + uiText("help.field.startup");
            SetWindowTextW(settingNote_, message.c_str());
        } else if (id >= 400 && id <= 414) {
            const string key = string("help.field.") + keys[id - 400];
            SetWindowTextW(settingNote_, uiText(key.c_str()));
        } else SetWindowTextW(settingNote_, uiText(settingsGroup_ == 2 ? "settings.preferences_note" : g_cfg.errors().empty() ? "settings.note" : "first_run.note"));
    }

    /// Build localized help using generic callback templates and address-free inspection results.
    void refreshHelp() {
        if (!helpText_) return;
        const int topic = static_cast<int>(SendMessageW(helpTopic_, CB_GETCURSEL, 0, 0));
        wstring body;
        if (topic == 1) {
            const char* fields[] = {"phone_address_mode", "phone_mac", "phone_ip", "phone_port", "user", "password", "local_ip", "control", "event", "udp_start", "udp_end", "timeout", "directory", "record_folder_mode", "record_folder_pattern", "close", "health", "external", "language", "notifications", "startup"};
            const char* configs[] = {"phone_address_mode", "phone_mac", "phone_ip", "phone_port", "phone_user", "phone_pass_dpapi", "advertise_ip", "control_port", "hook_port", "udp_port_start", "udp_port_end", "idle_timeout", "record_dir", "record_folder_mode", "record_folder_pattern", "close_action", "health_interval", "hook_port=0", "config.ini.ui", "tray_notifications", "Windows"};
            for (size_t index = 0; index < std::size(fields); ++index) {
                body += wstring(uiText((string("field.") + fields[index]).c_str())) + L" [" + utf8ToWide(configs[index]) + L"]\r\n";
                body += uiText((string("help.field.") + fields[index]).c_str()); body += L"\r\n\r\n";
            }
        } else if (topic == 2) {
            body = uiText("help.ports.body"); body += L"\r\n";
            if (SendMessageW(externalChoice_, BM_GETCHECK, 0, 0) == BST_CHECKED) body += uiText("help.port_external");
            else {
                body += uiText("help.port_preview"); body += L"\r\n\r\n";
                const wstring base = L"http://<PC_IP>:<EVENT_PORT>/event?ev=";
                body += wstring(uiText("help.port_est")) + L":\r\n" + base + L"est\r\n\r\n";
                body += wstring(uiText("help.port_end")) + L":\r\n" + base + L"end\r\n\r\n";
                body += uiText("help.port_control");
            }
        } else if (topic == 5) body = compatibilityText();
        else if (topic == 6) body = uiText("help.setup.body");
        else body = uiText(topic == 3 ? "help.devices.body" : topic == 4 ? "help.diagnostics.body" : "help.start.body");
        if (topic == 0) body += L"\r\n\r\n" + wstring(uiText("rename.help"));
        SetWindowTextW(helpText_, body.c_str());
        const auto report = compatibilitySnapshot(); lastCompatRevision_ = report.revision;
        lastCompatRuntime_ = compatibilityRuntimeVerified(report, diagnosticSnapshot(), g_sessionCount.load());
        layout();
    }

    /// Format the inspection using only resource keys, never inspected addresses or callback values.
    wstring compatibilityText() const {
        const auto report = compatibilitySnapshot();
        const bool runtime = compatibilityRuntimeVerified(report, diagnosticSnapshot(), g_sessionCount.load());
        const string verdict = runtime ? "runtime_verified" : report.verdict;
        wstring body = wstring(uiText("compat.title")) + L"\r\n\r\n" + uiText(("compat." + verdict).c_str());
        body += L"\r\n\r\n"; body += uiText("compat.scope");
        if (!report.checkedAt.empty()) body += L"\r\n" + wstring(uiText("compat.checked")) + L": " + utf8ToWide(report.checkedAt);
        body += L"\r\n\r\n";
        for (const auto& item : report.items) {
            const bool media = runtime && item.key == "runtime";
            body += L"[" + wstring(uiText(("compat." + string(media ? "pass" : item.status)).c_str())) + L"] ";
            body += uiText(("compat.item." + item.key).c_str()); body += L"\r\n";
            body += uiText(("compat.advice." + string(media ? "runtime_ok" : item.advice)).c_str()); body += L"\r\n\r\n";
        }
        body += uiText("compat.no_changes"); body += L"\r\n\r\n"; body += uiText("compat.next");
        return body;
    }

    /// Identify connection drafts so a new user never checks a different saved phone by mistake.
    bool connectionDraftChanged() const {
        const int hook = SendMessageW(externalChoice_, BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : configurationInteger(text(fields_[6]));
        return addressModeDraft() != g_cfg.phoneAddressMode || trim(text(addressMac_)) != g_cfg.phoneMac || trim(text(fields_[0])) != g_cfg.phoneIp || configurationInteger(text(fields_[1])) != g_cfg.phonePort ||
            trim(text(fields_[2])) != g_cfg.phoneUser || text(fields_[3]) != g_cfg.phonePass || trim(text(fields_[4])) != g_cfg.advertiseIp ||
            configurationInteger(text(fields_[5])) != g_cfg.controlPort || hook != g_cfg.hookPort ||
            configurationInteger(text(fields_[7])) != g_cfg.udpPortStart || configurationInteger(text(fields_[8])) != g_cfg.udpPortEnd ||
            configurationInteger(text(fields_[9])) != g_cfg.idleTimeoutSec || trim(text(fields_[10])) != g_cfg.recordDir || storageModeDraft() != g_cfg.recordFolderMode || text(storagePattern_) != g_cfg.recordFolderPattern;
    }

    /// Suggest only blank/previously suggested drafts; keep adapter enumeration off the GUI thread.
    void requestLocalIpSuggestion() {
        if (g_offline || !fields_[0] || !fields_[4] || localIpWorker_.joinable()) return;
        const string current = trim(text(fields_[4])), phone = trim(text(fields_[0]));
        if ((!current.empty() && current != suggestedLocalIp_) || (!phone.empty() && !usableLocalIpv4(phone))) return;
        localIpQuery_ = phone;
        localIpWorker_ = std::thread([this, phone]() {
            localIpResult_ = detectLocalIpv4(phone);
            PostMessageW(window_, WM_APP + 25, 0, 0);
        });
    }

    /// Discard stale phone results and preserve manual edits made while the lookup was running.
    void completeLocalIpSuggestion() {
        if (!localIpWorker_.joinable()) return;
        localIpWorker_.join();
        if (g_stopping) return;
        if (localIpQuery_ != trim(text(fields_[0]))) { requestLocalIpSuggestion(); return; }
        const string current = trim(text(fields_[4]));
        if (!current.empty() && current != suggestedLocalIp_) return;
        changingLocalIp_ = true;
        SetWindowTextW(fields_[4], utf8ToWide(localIpResult_).c_str());
        changingLocalIp_ = false;
        suggestedLocalIp_ = localIpResult_;
        refreshSettingsVisibility();
    }

    /// Queue the explicit read-only inspection after explaining any unsaved or unapplied settings.
    void requestCompatibility() {
        if (restartNeeded_ || connectionDraftChanged()) {
            CompatibilityReport report; report.verdict = restartNeeded_ ? "restart" : "save_required"; report.checkedAt = diagnosticTime();
            publishCompatibility(std::move(report));
        } else g_commands.enqueue("compatibility");
        switchPage(4); SendMessageW(helpTopic_, CB_SETCURSEL, 5, 0); refreshHelp();
    }

    /// Change every application-owned label in place, retaining sockets, recording, drafts and playback.
    void changeLanguage() {
        const int selected = static_cast<int>(SendMessageW(languageChoice_, CB_GETCURSEL, 0, 0));
        if (selected < 0 || selected > 2) return;
        g_uiLanguage = selected;
        for (const auto& child : controls_) if (!child.textKey.empty()) SetWindowTextW(child.window, uiText(child.textKey.c_str()));
        for (const auto& listing : columnKeys_) for (size_t column = 0; column < listing.second.size(); ++column) {
            LVCOLUMNW value{}; value.mask = LVCF_TEXT; value.pszText = const_cast<wchar_t*>(uiText(listing.second[column].c_str()));
            ListView_SetColumn(listing.first, static_cast<int>(column), &value);
        }
        translateCombo(logFilter_, {"filter.all", "filter.problems"});
        translateCombo(addressModeChoice_, {"address.fixed", "address.dhcp"});
        refreshAddressMode();
        translateCombo(storageMode_, {"storage.flat", "storage.daily", "storage.weekly", "storage.custom"});
        refreshSettingsVisibility();
        translateCombo(closeChoice_, {"close.tray", "close.minimize", "close.exit"});
        translateCombo(intervalChoice_, {"interval.off", "interval.30", "interval.60", "interval.120", "interval.300"}, true);
        translateCombo(helpTopic_, {"help.topic.start", "help.topic.fields", "help.topic.ports", "help.topic.devices", "help.topic.diagnostics", "help.topic.compatibility", "help.topic.setup"});
        translateCombo(updateInterval_, {"update.off", "update.hour1", "update.hour6", "update.hour12", "update.hour24"});
        refreshUpdates(true);
        SendMessageW(recordingSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("search.recordings")));
        SendMessageW(logSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("search.logs")));
        if (!player_.loaded()) SetWindowTextW(playerCaption_, uiText("player.empty"));
        const wstring title = L"PhoneRecord " + utf8ToWide(kProductVersion) + L"  ·  " + uiText("app.subtitle"); SetWindowTextW(window_, title.c_str());
        showFieldHelp(focusedSetting_); refreshHelp(); updatePlayer(); refreshLogs(true); refreshRecordings(false); layout();
        RedrawWindow(window_, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (!saveInterfaceLanguage(uiLanguageCode())) MessageBoxW(window_, uiText("message.language_save_failed"), L"PhoneRecord", MB_OK | MB_ICONWARNING);
    }

    /// Draw one scaled text rectangle with native ellipsis/wrapping behavior.
    void drawText(HDC dc, const wstring& value, int x, int y, int w, int h, COLORREF color, HFONT font, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        RECT area{px(x), px(y), px(x + w), px(y + h)};
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
        HGDIOBJ old = SelectObject(dc, font); DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &area, format | DT_NOPREFIX); SelectObject(dc, old);
    }

    /// Draw a subtle, rounded surface card using owned temporary GDI objects.
    void card(HDC dc, int x, int y, int w, int h) {
        HBRUSH fill = CreateSolidBrush(RGB(255, 255, 255)); HPEN border = CreatePen(PS_SOLID, 1, RGB(226, 232, 241));
        HGDIOBJ oldBrush = SelectObject(dc, fill), oldPen = SelectObject(dc, border);
        RoundRect(dc, px(x), px(y), px(x + w), px(y + h), px(12), px(12));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(fill); DeleteObject(border);
    }

    /// Resolve a stage to concise operator-facing text without assuming that HTTP implies audio.
    const wchar_t* stageText(const DiagnosticSnapshot& state) const {
        if (g_activeRecordings > 0) return uiText("stage.recording");
        if (!state.callActive && g_sessionCount == 0) return uiText("status.waiting");
        if (state.stage == RecordingStage::Failed) return uiText("stage.failed");
        if (state.callActive || g_sessionCount > 0) return uiText("stage.control_wait");
        return uiText("status.waiting");
    }

    /// Paint the shell and overview using a memory DC to avoid flicker.
    void paint(HDC external = NULL) {
        PAINTSTRUCT paint{}; HDC target = external ? external : BeginPaint(window_, &paint);
        RECT bounds{}; GetClientRect(window_, &bounds);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
        HGDIOBJ old = SelectObject(dc, bitmap); FillRect(dc, &bounds, background_);
        RECT sidebar{0, 0, px(176), bounds.bottom}; HBRUSH dark = CreateSolidBrush(RGB(20, 32, 51)); FillRect(dc, &sidebar, dark); DeleteObject(dark);
        drawText(dc, L"PhoneRecord", 20, 24, 152, 32, RGB(255, 255, 255), valueFont_);
        drawText(dc, uiText("app.subtitle"), 21, 62, 150, 42, RGB(153, 175, 202), smallFont_, DT_LEFT | DT_TOP | DT_WORDBREAK);
        drawText(dc, uiText("field.language"), 14, height() - 164, 148, 22, RGB(153, 175, 202), smallFont_);
        drawText(dc, L"v" + utf8ToWide(kProductVersion), 21, height() - 42, versionLabelWidth(dc) + 4, 22, RGB(153, 175, 202), smallFont_);
        const char* titles[] = {"nav.overview", "nav.recordings", "nav.diagnostics", "nav.settings", "nav.help", "nav.updates", "speech.module_live", "app.title", "speech.module_history"};
        const char* subtitles[] = {"overview.subtitle", "recordings.subtitle", "diagnostics.subtitle", "settings.subtitle", "help.subtitle", "update.subtitle", "speech.module_live_subtitle", "app.subtitle", "speech.module_history_subtitle"};
        drawText(dc, uiText(titles[page_]), 200, 22, width() - 430, 40, RGB(23, 40, 65), titleFont_);
        drawText(dc, uiText(subtitles[page_]), 200, 66, width() - 224, 25, RGB(100, 116, 139), font_);
        const auto state = diagnosticSnapshot();
        const bool ready = state.controlListening && (state.eventListening || g_cfg.hookPort == 0) && (state.authenticated || g_cfg.hookPort == 0) && g_diagnosticLog.storageError().empty() && state.localHealth.status != 2 && state.phoneHealth.status != 2;
        const bool configured = compatibilitySnapshot().verdict == "ready" || state.attemptRtp;
        const wchar_t* badge = g_offline ? uiText("status.offline") : restartNeeded_ ? uiText("status.saved_restart") : !ready ? uiText("status.attention") : g_cfg.hookPort == 0 ? uiText("status.receiver") : uiText(configured ? "status.ready" : "status.setup_needed");
        if (page_ != 6 && page_ != 8) drawText(dc, badge, width() - 226, 30, 202, 28, ready && (configured || g_cfg.hookPort == 0) && !g_offline ? RGB(16, 127, 101) : RGB(179, 102, 10), font_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (page_ == 0) paintOverview(dc, state);
        if (page_ == 1) paintPlayer(dc);
        if (page_ == 1) drawText(dc, uiText("footer.evidence"), 200, height() - 36, width() - 224, 24, RGB(100, 116, 139), smallFont_);
        if (page_ == 2) drawText(dc, uiText("footer.logs"), 200, height() - 36, width() - 224, 24, RGB(100, 116, 139), smallFont_);
        BitBlt(target, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); if (!external) EndPaint(window_, &paint);
    }

    /// Draw one scalable health lamp and its independent latest-check result.
    void healthLamp(HDC dc, int x, int y, int width, const char* label, const HealthResult& health, bool deferred) {
        const COLORREF color = health.status == 1 ? RGB(24, 151, 105) : health.status == 2 ? RGB(210, 66, 61) : health.status == 3 ? RGB(221, 154, 28) : RGB(154, 166, 183);
        HBRUSH fill = CreateSolidBrush(color); HGDIOBJ old = SelectObject(dc, fill), pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, px(x), px(y + 5), px(x + 12), px(y + 17)); SelectObject(dc, old); SelectObject(dc, pen); DeleteObject(fill);
        const wchar_t* result = uiText(health.status == 1 ? "health.ok" : health.status == 2 ? "health.failed" : health.status == 3 ? "health.checking" : "health.unknown");
        drawText(dc, wstring(uiText(label)) + L"  ·  " + result + (deferred ? wstring(L"  ·  ") + uiText("health.deferred") : L""), x + 22, y, width - 22, 25, RGB(31, 51, 77), font_);
        const wstring time = health.checkedAt.empty() ? uiText("health.not_checked") : wstring(uiText("health.checked")) + L" " + utf8ToWide(health.checkedAt.substr(11, 8));
        drawText(dc, time, x + 22, y + 26, width - 22, 20, RGB(100, 116, 139), smallFont_);
    }

    /// Keep current call status, the last call's evidence and service health visibly separate.
    void paintOverview(HDC dc, const DiagnosticSnapshot& state) {
        const int available = width() - 224, each = (available - 30) / 4;
        const bool active = state.callActive || g_sessionCount > 0 || g_activeRecordings > 0;
        const char* labels[] = {"card.recording", "card.control", "card.event", "card.media"};
        const wstring values[] = {stageText(state), std::to_wstring(g_cfg.controlPort) + L"  ·  " + uiText(state.controlListening ? "status.listening" : "status.not_listening"), g_cfg.hookPort == 0 ? uiText("status.external") : std::to_wstring(g_cfg.hookPort) + L"  ·  " + uiText(state.eventListening ? "status.listening" : "status.not_listening"), active && state.attemptPackets ? std::to_wstring(state.attemptPackets) + L" RTP" : uiText("status.idle")};
        for (int index = 0; index < 4; ++index) { const int x = 200 + index * (each + 10); card(dc, x, 108, each, 84); drawText(dc, uiText(labels[index]), x + 14, 119, each - 28, 22, RGB(100, 116, 139), smallFont_); drawText(dc, values[index], x + 14, 146, each - 28, 30, RGB(31, 51, 77), valueFont_); }
        card(dc, 200, 208, available, 96);
        const wchar_t* caption = uiText(!active && state.attempts > 0 ? "section.last_pipeline" : "section.pipeline");
        drawText(dc, caption, 216, 215, 330, 23, RGB(70, 88, 112), smallFont_);
        if (!active && state.attempts > 0) drawText(dc, uiText(state.attemptWav && state.quality != "unknown" ? "stage.complete" : "stage.previous_incomplete"), 560, 215, available - 376, 23, RGB(100, 116, 139), smallFont_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        const char* stages[] = {"stage.event", "stage.http", "stage.tcp", "stage.reply", "stage.rtp", "stage.wav", "stage.quality"};
        const bool evidence[] = {state.attempts > 0, state.attemptHttp, state.attemptTcp, state.attemptReply, state.attemptRtp, state.attemptWav, state.quality != "unknown"};
        const int step = (available - 30) / 7;
        for (int index = 0; index < 7; ++index) {
            const int x = 216 + index * step;
            HBRUSH brush = CreateSolidBrush(evidence[index] ? RGB(43, 105, 230) : RGB(222, 229, 240)); HGDIOBJ old = SelectObject(dc, brush); HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, px(x + step / 2 - 7), px(247), px(x + step / 2 + 7), px(261)); SelectObject(dc, old); SelectObject(dc, pen); DeleteObject(brush);
            drawText(dc, uiText(stages[index]), x, 269, step, 23, evidence[index] ? RGB(43, 105, 230) : RGB(119, 135, 156), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        card(dc, 200, 318, available, 128);
        drawText(dc, uiText("section.health"), 216, 325, 310, 22, RGB(70, 88, 112), smallFont_);
        const int interval = g_healthIntervalSec.load();
        const wstring schedule = interval > 0 ? wstring(uiText("health.every")) + std::to_wstring(interval) + uiText("interval.seconds") : uiText("interval.off");
        drawText(dc, schedule, 540, 325, available - 356, 22, RGB(100, 116, 139), smallFont_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        healthLamp(dc, 216, 352, (available - 32) / 2, "health.local", state.localHealth, false);
        healthLamp(dc, 216 + (available - 32) / 2, 352, (available - 32) / 2, "health.phone", state.phoneHealth, active);
        string code = state.code;
        if (state.localHealth.status == 2) code = state.localHealth.code;
        else if (state.phoneHealth.status == 2) code = state.phoneHealth.code;
        else if (g_offline) code = "OFFLINE_MODE";
        else if (g_cfg.hookPort == 0) code = "EXTERNAL_TRIGGER_MODE";
        else if (!active && state.authenticated && state.controlListening && state.eventListening && state.localHealth.status == 1) code = compatibilitySnapshot().verdict == "ready" || state.attemptRtp ? "AUTO_RECORD_READY" : "PHONE_SETUP_REQUIRED";
        if (!g_diagnosticLog.storageError().empty()) code = g_diagnosticLog.storageError();
        const wstring guidance = diagnosticHint(code);
        drawText(dc, guidance, 216, 410, available - 32, 24, RGB(70, 88, 112), smallFont_);
        drawText(dc, uiText("section.recent"), 200, 506, available, 24, RGB(70, 88, 112), smallFont_);
        const wstring footer = utf8ToWide(state.changedAt) + L"   ·   " + std::to_wstring(state.files) + uiText("overview.saved_suffix");
        drawText(dc, footer, 200, height() - 34, available, 22, RGB(100, 116, 139), smallFont_);
    }

    /// Draw the in-application player surface; its child controls provide keyboard navigation.
    void paintPlayer(HDC dc) { card(dc, 200, height() - 178, width() - 224, 134); }

    /// Format a playback cursor or duration as minutes and seconds.
    wstring playbackTime(DWORD milliseconds) const { const DWORD seconds = milliseconds / 1000; wchar_t result[40]; swprintf_s(result, L"%lu:%02lu", seconds / 60, seconds % 60); return result; }

    /// Refresh playback buttons and cursor without changing recording or network state.
    void updatePlayer() {
        const bool ok = player_.pump();
        const bool loaded = player_.loaded();
        EnableWindow(GetDlgItem(window_, 216), loaded); EnableWindow(GetDlgItem(window_, 217), loaded); EnableWindow(playerSeek_, loaded);
        SetWindowTextW(GetDlgItem(window_, 216), uiText(player_.playing() ? "player.pause" : "player.resume"));
        SetWindowTextW(playerTime_, (playbackTime(player_.position()) + L" / " + playbackTime(player_.length())).c_str());
        if (!seeking_) SendMessageW(playerSeek_, TBM_SETPOS, TRUE, player_.length() ? static_cast<LPARAM>(static_cast<uint64_t>(player_.position()) * 1000 / player_.length()) : 0);
        if (!ok) showPlayerError();
    }

    /// Release a failed playback session and display its localized cause and technical code once.
    void showPlayerError() {
        const wchar_t* reason = uiText(player_.errorKey());
        const wstring message = wstring(reason) + L"\r\n\r\n" + player_.errorDetails();
        player_.close(); updatePlayer(); SetWindowTextW(playerCaption_, reason);
        MessageBoxW(window_, message.c_str(), L"PhoneRecord", MB_OK | MB_ICONWARNING);
    }

    /// Draw the GitHub mark into a device rectangle. The path is the 24-unit octocat, even-odd filled.
    void drawGitHubMark(HDC dc, RECT area, COLORREF color) const {
        static const int curves[] = {
            5370, 297, 0, 5670, 0, 12297, 0, 17600, 3438, 22097, 8205, 23682,
            8805, 23795, 9025, 23424, 9025, 23105, 9025, 22820, 9015, 22065, 9010, 21065,
            5672, 21789, 4968, 19455, 4968, 19455, 4422, 18070, 3633, 17700, 3633, 17700,
            2546, 16956, 3717, 16971, 3717, 16971, 4922, 17055, 5555, 18207, 5555, 18207,
            6625, 20042, 8364, 19512, 9050, 19205, 9158, 18429, 9467, 17900, 9810, 17600,
            7145, 17300, 4344, 16268, 4344, 11670, 4344, 10360, 4809, 9290, 5579, 8450,
            5444, 8147, 5039, 6927, 5684, 5274, 5684, 5274, 6689, 4952, 8984, 6504,
            9944, 6237, 10964, 6105, 11984, 6099, 13004, 6105, 14024, 6237, 14984, 6504,
            17264, 4952, 18269, 5274, 18269, 5274, 18914, 6927, 18509, 8147, 18389, 8450,
            19154, 9290, 19619, 10360, 19619, 11670, 19619, 16280, 16814, 17295, 14144, 17590,
            14564, 17950, 14954, 18686, 14954, 19810, 14954, 21416, 14939, 22706, 14939, 23096,
            14939, 23411, 15149, 23786, 15764, 23666, 20565, 22092, 24000, 17592, 24000, 12297,
            24000, 5670, 18627, 297, 12000, 297
        };
        static_assert(sizeof(curves) / sizeof(curves[0]) == 150, "GitHub mark cubic count");
        const int mark = px(20);
        const int insetX = std::max(0, (static_cast<int>(area.right - area.left) - mark) / 2);
        const int insetY = std::max(0, (static_cast<int>(area.bottom - area.top) - mark) / 2);
        InflateRect(&area, -insetX, -insetY);
        const int width = area.right - area.left, height = area.bottom - area.top;
        if (width <= 1 || height <= 1) return;
        const int spanX = width - 1, spanY = height - 1;
        auto map = [&](int value, int origin, int span) { return origin + MulDiv(value, span, 24000); };
        if (!BeginPath(dc)) return;
        MoveToEx(dc, map(12000, area.left, spanX), map(297, area.top, spanY), NULL);
        POINT points[75];
        int count = 0;
        for (int index = 0; index < 150; index += 6) {
            points[count++] = POINT{map(curves[index], area.left, spanX), map(curves[index + 1], area.top, spanY)};
            points[count++] = POINT{map(curves[index + 2], area.left, spanX), map(curves[index + 3], area.top, spanY)};
            points[count++] = POINT{map(curves[index + 4], area.left, spanX), map(curves[index + 5], area.top, spanY)};
        }
        PolyBezierTo(dc, points, count);
        EndPath(dc);
        const int previous = SetPolyFillMode(dc, ALTERNATE);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        FillPath(dc);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush);
        SetPolyFillMode(dc, previous);
    }

    /// Draw a native button with consistent spacing, disabled state and keyboard focus.
    void drawButton(DRAWITEMSTRUCT* item) {
        const int id = static_cast<int>(item->CtlID);
        if (id == 106) {
            HBRUSH surface = CreateSolidBrush(RGB(20, 32, 51));
            FillRect(item->hDC, &item->rcItem, surface); DeleteObject(surface);
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            drawGitHubMark(item->hDC, item->rcItem, pressed ? RGB(255, 255, 255) : RGB(153, 175, 202));
            if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
            return;
        }
        const bool tab = id >= 432 && id <= 434;
        const bool nav = (id >= 100 && id <= 105) || id == 107 || id == 109 || id == 902, active = (id >= 100 && id <= 105 && id - 100 == page_) || (id == 109 && page_ == 8) || (id == 107 && page_ == 6) || (tab && id - 432 == settingsGroup_);
        const bool disabled = (item->itemState & ODS_DISABLED) != 0, pressed = (item->itemState & ODS_SELECTED) != 0;
        COLORREF fill = nav ? active ? RGB(43, 105, 230) : RGB(20, 32, 51) : id == 500 || id == 420 || (tab && active) ? RGB(43, 105, 230) : RGB(255, 255, 255);
        if (pressed) fill = nav || id == 500 || id == 420 ? RGB(29, 79, 180) : RGB(232, 239, 250);
        HBRUSH surface = CreateSolidBrush(nav ? RGB(20, 32, 51) : RGB(246, 248, 252));
        FillRect(item->hDC, &item->rcItem, surface); DeleteObject(surface);
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, nav || id == 500 || id == 420 ? fill : RGB(211, 221, 235));
        HGDIOBJ oldBrush = SelectObject(item->hDC, brush), oldPen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, px(8), px(8));
        SelectObject(item->hDC, oldBrush); SelectObject(item->hDC, oldPen); DeleteObject(brush); DeleteObject(pen);
        wchar_t label[128]{}; GetWindowTextW(item->hwndItem, label, 128); SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, disabled ? RGB(142, 153, 169) : nav || id == 500 || id == 420 || (tab && active) ? RGB(255, 255, 255) : RGB(42, 62, 90));
        HGDIOBJ font = SelectObject(item->hDC, font_); RECT area = item->rcItem; DrawTextW(item->hDC, label, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); SelectObject(item->hDC, font);
        if (item->itemState & ODS_FOCUS) { InflateRect(&area, -px(4), -px(4)); DrawFocusRect(item->hDC, &area); }
    }

    /// Insert a single native list row; temporary Unicode buffers are copied by the control.
    void row(HWND listWindow, int index, const std::vector<wstring>& values) {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index; item.pszText = const_cast<wchar_t*>(values[0].c_str());
        const int actual = ListView_InsertItem(listWindow, &item);
        for (size_t column = 1; column < values.size(); ++column) ListView_SetItemText(listWindow, actual, static_cast<int>(column), const_cast<wchar_t*>(values[column].c_str()));
    }

    /// Refresh only when the log sequence or user filter changes, retaining selected evidence.
    void refreshLogs(bool force) {
        const uint64_t sequence = g_diagnosticLog.sequence(); if (!force && sequence == lastLog_) return;
        lastLog_ = sequence; const auto all = g_diagnosticLog.entries();
        if (page_ == 0 || force) {
            const int previousTop = ListView_GetTopIndex(recent_);
            const uint64_t topSequence = previousTop >= 0 && previousTop < static_cast<int>(recentSequences_.size()) ? recentSequences_[previousTop] : 0;
            recentSequences_.clear();
            SendMessageW(recent_, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(recent_);
            std::vector<const DiagnosticEntry*> issues;
            for (const auto& item : all) if (item.level == "WARN" || item.level == "ERROR") issues.push_back(&item);
            const size_t first = issues.size() > 100 ? issues.size() - 100 : 0;
            for (size_t index = first; index < issues.size(); ++index) {
                const auto& item = *issues[index];
                const string code = item.text.substr(0, item.text.find(' '));
                row(recent_, static_cast<int>(recentSequences_.size()), {utf8ToWide(item.time.substr(11)), utf8ToWide(item.level), diagnosticHint(code)});
                recentSequences_.push_back(item.sequence);
            }
            if (recentSequences_.empty()) row(recent_, 0, {L"", L"", uiText("overview.no_issues")});
            // Re-enable redraw before reading the native scroll position; while it is
            // disabled, a rebuilt list can report the old cached top index.
            SendMessageW(recent_, WM_SETREDRAW, TRUE, 0);
            ShowWindow(recent_, page_ == 0 ? SW_SHOW : SW_HIDE);
            if (SendMessageW(followRecent_, BM_GETCHECK, 0, 0) == BST_CHECKED && !recentSequences_.empty()) ListView_EnsureVisible(recent_, static_cast<int>(recentSequences_.size() - 1), FALSE);
            else for (size_t index = 0; index < recentSequences_.size(); ++index) if (recentSequences_[index] == topSequence) { RECT bounds{}; if (ListView_GetItemRect(recent_, 0, &bounds, LVIR_BOUNDS)) ListView_Scroll(recent_, 0, (static_cast<int>(index) - ListView_GetTopIndex(recent_)) * (bounds.bottom - bounds.top)); break; }
            InvalidateRect(recent_, NULL, TRUE);
        }
        if (page_ != 2 && !force) return;
        const string search = toLower(text(logSearch_)); const bool problems = SendMessageW(logFilter_, CB_GETCURSEL, 0, 0) == 1;
        const int previousTop = ListView_GetTopIndex(diagnostics_);
        const int previousCount = ListView_GetItemCount(diagnostics_);
        const bool followedTail = previousTop + ListView_GetCountPerPage(diagnostics_) >= previousCount - 1;
        const uint64_t topSequence = previousTop >= 0 && previousTop < static_cast<int>(visibleLogs_.size()) ? visibleLogs_[previousTop].sequence : 0;
        const int selected = ListView_GetNextItem(diagnostics_, -1, LVNI_SELECTED);
        const uint64_t selectedSequence = selected >= 0 && selected < static_cast<int>(visibleLogs_.size()) ? visibleLogs_[selected].sequence : 0;
        visibleLogs_.clear();
        for (const auto& event : all) if ((!problems || event.level == "WARN" || event.level == "ERROR") && (search.empty() || toLower(event.text).find(search) != string::npos || event.level.find(search) != string::npos)) visibleLogs_.push_back(event);
        if (visibleLogs_.size() > 500) visibleLogs_.erase(visibleLogs_.begin(), visibleLogs_.end() - 500);
        SendMessageW(diagnostics_, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(diagnostics_);
        for (size_t index = 0; index < visibleLogs_.size(); ++index) { const auto& item = visibleLogs_[index]; row(diagnostics_, static_cast<int>(index), {std::to_wstring(item.sequence), utf8ToWide(item.time.substr(11)), utf8ToWide(item.level), utf8ToWide(item.text)}); if (item.sequence == selectedSequence) ListView_SetItemState(diagnostics_, static_cast<int>(index), LVIS_SELECTED, LVIS_SELECTED); }
        SendMessageW(diagnostics_, WM_SETREDRAW, TRUE, 0); InvalidateRect(diagnostics_, NULL, TRUE);
        if (!visibleLogs_.empty()) {
            if (followedTail && !selectedSequence) ListView_EnsureVisible(diagnostics_, static_cast<int>(visibleLogs_.size() - 1), FALSE);
            else for (size_t index = 0; index < visibleLogs_.size(); ++index) if (visibleLogs_[index].sequence == topSequence) { RECT rowArea{}; if (ListView_GetItemRect(diagnostics_, 0, &rowArea, LVIR_BOUNDS)) ListView_Scroll(diagnostics_, 0, static_cast<int>(index) * (rowArea.bottom - rowArea.top)); break; }
        }
    }

    /// Scan completed recording metadata on page entry or explicit refresh, preserving selection.
    void refreshRecordings(bool scan) {
        const int selected = ListView_GetNextItem(recordings_, -1, LVNI_SELECTED);
        const string file = selected >= 0 && selected < static_cast<int>(visibleCalls_.size()) ? visibleCalls_[selected].file : "";
        if (scan) calls_ = scanRecordings();
        const string search = toLower(text(recordingSearch_)); visibleCalls_.clear();
        for (const auto& item : calls_) if (search.empty() || toLower(item.title + " " + item.name + " " + item.remote + " " + item.date + " " + item.file).find(search) != string::npos) visibleCalls_.push_back(item);
        SendMessageW(recordings_, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(recordings_);
        for (size_t index = 0; index < visibleCalls_.size(); ++index) {
            const auto& item = visibleCalls_[index];
            const string quality = "quality." + item.quality;
            const wchar_t* source = item.source == "configured_phone" ? uiText("source.phone") : item.source == "test_or_other_peer" ? uiText("source.test") : uiText("source.unknown");
            row(recordings_, static_cast<int>(index), {utf8ToWide(item.date + " " + item.clock), utf8ToWide(item.title.empty() ? item.name.empty() ? item.remote : item.name : item.title), uiText(item.direction == "in" ? "direction.in" : "direction.out"), utf8ToWide(item.name.empty() ? item.remote : item.name + "  " + item.remote), utf8ToWide(durationText(item)), uiText(quality.c_str()), source, std::to_wstring(item.size / 1024) + L" KiB"});
            if (item.file == file) ListView_SetItemState(recordings_, static_cast<int>(index), LVIS_SELECTED, LVIS_SELECTED);
        }
        SendMessageW(recordings_, WM_SETREDRAW, TRUE, 0); InvalidateRect(recordings_, NULL, TRUE);
        EnableWindow(GetDlgItem(window_, 221), ListView_GetNextItem(recordings_, -1, LVNI_SELECTED) >= 0);
    }

    /// Carry the selected recording identity and metadata generation through one modal edit.
    struct RenameRequest {
        DesktopView* owner = NULL;
        wstring path, identity;
        string fileId;
        RecordingTitle previous;
    };

    /// Localize the native title editor and keep failed saves open for correction or cancellation.
    static INT_PTR CALLBACK renameProcedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* request = reinterpret_cast<RenameRequest*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            request = reinterpret_cast<RenameRequest*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            request->owner->renameDialog_ = dialog;
            SetWindowTextW(dialog, uiText("rename.dialog"));
            SetDlgItemTextW(dialog, IDC_RENAME_INFO, request->identity.c_str());
            SetDlgItemTextW(dialog, IDC_RENAME_LABEL, uiText("column.title"));
            SetDlgItemTextW(dialog, IDC_RENAME_HINT, uiText("rename.hint"));
            SetDlgItemTextW(dialog, IDC_RENAME_RESET, uiText("rename.reset"));
            SetDlgItemTextW(dialog, IDOK, uiText("rename.save")); SetDlgItemTextW(dialog, IDCANCEL, uiText("rename.cancel"));
            SendDlgItemMessageW(dialog, IDC_RENAME_TITLE, EM_SETLIMITTEXT, 240, 0);
            SetDlgItemTextW(dialog, IDC_RENAME_TITLE, request->previous.value.c_str());
            SendDlgItemMessageW(dialog, IDC_RENAME_TITLE, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(uiText("rename.placeholder")));
            SetFocus(GetDlgItem(dialog, IDC_RENAME_TITLE)); return FALSE;
        }
        if (message == WM_COMMAND && request) {
            if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
            if (LOWORD(wparam) == IDC_RENAME_RESET) { SetDlgItemTextW(dialog, IDC_RENAME_TITLE, L""); SetFocus(GetDlgItem(dialog, IDC_RENAME_TITLE)); return TRUE; }
            if (LOWORD(wparam) == IDOK) {
                const int count = GetWindowTextLengthW(GetDlgItem(dialog, IDC_RENAME_TITLE));
                wstring value(static_cast<size_t>(count) + 1, L'\0'); GetDlgItemTextW(dialog, IDC_RENAME_TITLE, &value[0], count + 1); value.resize(count);
                DWORD error = 0; const TitleResult result = saveRecordingTitle(request->path, value, request->previous, error);
                if (result == TitleResult::Saved) { wstring normalized; normalizeRecordingTitle(value, normalized); LOGI("CALL_TITLE_SAVED file_id=%s reset=%d", request->fileId.c_str(), normalized.empty()); EndDialog(dialog, IDOK); }
                else {
                    const char* key = result == TitleResult::Invalid ? "rename.invalid" : result == TitleResult::Busy ? "rename.busy" : result == TitleResult::Changed ? "rename.changed" : result == TitleResult::Metadata ? "rename.metadata" : "rename.failed";
                    LOGW("CALL_TITLE_SAVE_FAILED file_id=%s reason=%s win32=%lu", request->fileId.c_str(), key, error);
                    wstring explanation = uiText(key); if (error) explanation += L"\r\nWin32: " + std::to_wstring(error);
                    MessageBoxW(dialog, explanation.c_str(), uiText("rename.dialog"), MB_OK | MB_ICONWARNING);
                }
                return TRUE;
            }
        }
        if (message == WM_DESTROY && request) request->owner->renameDialog_ = NULL;
        if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    /// Edit a display title while retaining the WAV, ledger, contact and quality metadata.
    void renameRecording() {
        const int selected = ListView_GetNextItem(recordings_, -1, LVNI_SELECTED);
        if (selected < 0 || selected >= static_cast<int>(visibleCalls_.size())) return;
        const auto item = visibleCalls_[selected];
        RenameRequest request;
        request.owner = this;
        request.fileId = sessionFingerprint(item.file);
        request.path = utf8ToWide(g_cfg.recordDir + "\\" + item.file);
        request.identity = utf8ToWide(item.date + " " + item.clock) + L"  ·  " + uiText(item.direction == "in" ? "direction.in" : "direction.out") + L"\r\n" + utf8ToWide(item.remote + "  " + item.name);
        request.previous = readRecordingTitle(request.path);
        if (request.previous.error) { LOGW("CALL_TITLE_READ_FAILED win32=%lu", request.previous.error); MessageBoxW(window_, uiText("rename.metadata"), uiText("rename.dialog"), MB_OK | MB_ICONWARNING); return; }
        if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_RENAME_CALL), window_, renameProcedure, reinterpret_cast<LPARAM>(&request)) == IDOK) {
            refreshRecordings(true);
            if (player_.path() == wideToUtf8(request.path)) {
                const auto title = readRecordingTitle(request.path);
                SetWindowTextW(playerCaption_, title.value.empty() ? utf8ToWide(item.file).c_str() : title.value.c_str());
            }
        }
    }

    /// Return the selected stable file path and reject incomplete active recordings.
    string selectedFile() {
        const int selected = ListView_GetNextItem(recordings_, -1, LVNI_SELECTED);
        if (selected < 0 || selected >= static_cast<int>(visibleCalls_.size())) { MessageBoxW(window_, uiText("message.select"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return ""; }
        return g_cfg.recordDir + "\\" + visibleCalls_[selected].file;
    }

    /// Copy a Unicode diagnostic report using the native clipboard ownership contract.
    void copyReport(bool compatibility = false) {
        const wstring report = compatibility ? compatibilityText() : utf8ToWide(diagnosticReport());
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (report.size() + 1) * sizeof(wchar_t));
        if (!memory) return;
        void* buffer = GlobalLock(memory); if (!buffer) { GlobalFree(memory); return; }
        memcpy(buffer, report.c_str(), (report.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
        if (!OpenClipboard(window_)) { GlobalFree(memory); return; }
        EmptyClipboard(); const bool copied = SetClipboardData(CF_UNICODETEXT, memory) != NULL; CloseClipboard();
        if (!copied) GlobalFree(memory); else MessageBoxW(window_, uiText(compatibility ? "compat.copied" : "message.copied"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION);
    }

    /// Export a reviewable UTF-8 text report to a path selected by the operator.
    void exportReport() {
        wchar_t path[32768] = L"PhoneRecord-diagnostics.txt";
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path)); dialog.lpstrFilter = L"Text (*.txt)\0*.txt\0\0";
        dialog.lpstrTitle = uiText("dialog.export"); dialog.lpstrDefExt = L"txt"; dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;
        std::ofstream output(path, std::ios::binary | std::ios::trunc); output << diagnosticReport(); output.flush();
        MessageBoxW(window_, uiText(output ? "message.exported" : "message.export_failed"), L"PhoneRecord", MB_OK | (output ? MB_ICONINFORMATION : MB_ICONERROR));
    }

    /// Offer secondary recording actions for the clicked row or keyboard selection.
    void recordingContextMenu(LPARAM position) {
        POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
        if (point.x == -1 && point.y == -1) {
            const int selected = ListView_GetNextItem(recordings_, -1, LVNI_SELECTED);
            RECT bounds{};
            if (selected >= 0 && ListView_GetItemRect(recordings_, selected, &bounds, LVIR_BOUNDS)) point = {bounds.left + 20, bounds.bottom};
            else point = {16, 16};
            ClientToScreen(recordings_, &point);
        } else {
            POINT local = point; ScreenToClient(recordings_, &local);
            LVHITTESTINFO hit{}; hit.pt = local;
            const int index = ListView_HitTest(recordings_, &hit);
            ListView_SetItemState(recordings_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            if (index >= 0) ListView_SetItemState(recordings_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        const bool selected = ListView_GetNextItem(recordings_, -1, LVNI_SELECTED) >= 0;
        HMENU menu = CreatePopupMenu(); if (!menu) return;
        const UINT flags = MF_STRING | (selected ? 0 : MF_GRAYED);
        AppendMenuW(menu, flags, 211, uiText("button.play"));
        AppendMenuW(menu, flags, 221, uiText("button.rename"));
        AppendMenuW(menu, flags, 212, uiText("button.reveal"));
        AppendMenuW(menu, MF_STRING, 215, uiText("button.open_folder"));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (selected && !analysisWorker_.joinable() ? 0 : MF_GRAYED), 213, uiText("button.analyse"));
#ifdef _WIN64
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, flags, 225, uiText("speech.module_history"));
#endif
        const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, NULL);
        DestroyMenu(menu);
        if (action) command(static_cast<int>(action));
    }

    /// Review and apply supported phone settings through the existing serialized worker.
    void requestPhoneSetup() {
        if (setupDialog_ || g_commands.phoneSetupProgress().running) return;
        if (restartNeeded_ || connectionDraftChanged() || !g_cfg.errors().empty()) {
            MessageBoxW(window_, uiText("setup.direct_save_first"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return;
        }
        if (!g_commands.enqueue("setup_preview"))
            MessageBoxW(window_, uiText("setup.direct_unavailable"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION);
    }

    /// Display each worker completion once; the confirmation stays on the GUI thread.
    void refreshPhoneSetup() {
        const auto progress = g_commands.phoneSetupProgress();
        SetWindowTextW(GetDlgItem(window_, 424), uiText(progress.running ? "setup.direct_working" : "button.phone_setup_direct"));
        EnableWindow(GetDlgItem(window_, 424), !progress.running && !setupDialog_ && !g_offline &&
            !diagnosticSnapshot().callActive && g_sessionCount == 0 && !g_updateInstalling);
        if (progress.running || progress.revision == setupRevision_ || setupDialog_) return;
        setupRevision_ = progress.revision;
        const auto& result = progress.result;
        setupDialog_ = true;
        if (result.ok && !progress.apply && !result.changes.empty()) {
            wstring preview = uiText("setup.direct_preview");
            preview += L"\r\n\r\n" + utf8ToWide(phoneAddress() + ":" + std::to_string(g_cfg.phonePort));
            for (const auto& field : result.changes) preview += L"\r\n- " + wstring(uiText(("setup.field." + field).c_str()));
            preview += L"\r\n\r\n" + wstring(uiText("setup.direct_confirm"));
            const bool confirmed = MessageBoxW(window_, preview.c_str(), uiText("button.phone_setup_direct"),
                MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES;
            LOGI("PHONE_SETUP_CONFIRM accepted=%d", confirmed);
            if (confirmed && !g_stopping && IsWindow(window_)) {
                if (restartNeeded_ || connectionDraftChanged())
                    MessageBoxW(window_, uiText("setup.direct_save_first"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION);
                else if (!g_commands.enqueue("setup_apply", "manual", result.planId))
                    MessageBoxW(window_, uiText("setup.direct_unavailable"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION);
            }
        } else {
            wstring message = result.ok && progress.apply ? uiText("setup.direct_done") : uiText(("setup." + result.code).c_str());
            if (!result.cause.empty()) message += L"\r\n" + wstring(uiText(("setup." + result.cause).c_str()));
            if (!result.backup.empty()) message += L"\r\n\r\n" + wstring(uiText("setup.direct_backup")) + L"\r\n" + utf8ToWide(result.backup);
            MessageBoxW(window_, message.c_str(), uiText("button.phone_setup_direct"), MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            if (result.ok && !g_stopping && IsWindow(window_)) requestCompatibility();
        }
        setupDialog_ = false;
    }

    /// Bind the Windows login entry to this executable and the active configuration file.
    UserStartup currentStartup() const {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(NULL, executable, static_cast<DWORD>(std::size(executable)));
        return UserStartup(length && length < std::size(executable) ? executable : L"", g_configPath);
    }

    /// Reflect Windows registration on entry/activation, including registrations of another copy.
    void refreshStartup() {
        if (!startupChoice_) return;
        const auto state = currentStartup().read();
        SendMessageW(startupChoice_, BM_SETCHECK, state.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(startupChoice_, state.error == ERROR_SUCCESS);
        if (focusedSetting_ == 415) showFieldHelp(415);
    }

    /// Apply an explicit checkbox action immediately, independently of unsaved phone settings.
    void changeStartup() {
        const bool enabled = SendMessageW(startupChoice_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const LONG result = currentStartup().setEnabled(enabled);
        refreshStartup(); showFieldHelp(415);
        if (result != ERROR_SUCCESS) {
            LOGE("AUTOSTART_CHANGE_FAILED requested=%d win32=%ld", enabled, result);
            const wstring message = wstring(uiText(result == ERROR_FILENAME_EXCED_RANGE ? "startup.path_too_long" : "startup.failed")) + L"\r\nWin32: " + std::to_wstring(result);
            MessageBoxW(window_, message.c_str(), L"PhoneRecord", MB_OK | MB_ICONERROR);
        } else LOGI("AUTOSTART_CHANGED enabled=%d verified=1", enabled);
    }

    /// Validate a separate settings copy and preserve the live socket configuration until restart.
    void saveSettings() {
        Config candidate = g_cfg; candidate.formatError = false;
        candidate.phoneAddressMode = addressModeDraft(); candidate.phoneMac = trim(text(addressMac_));
        candidate.phoneIp = trim(text(fields_[0])); candidate.phonePort = configurationInteger(text(fields_[1]));
        candidate.phoneUser = trim(text(fields_[2])); candidate.phonePass = text(fields_[3]); candidate.advertiseIp = trim(text(fields_[4]));
        candidate.secretError = g_cfg.secretError && candidate.phonePass.empty();
        candidate.controlPort = configurationInteger(text(fields_[5])); candidate.hookPort = SendMessageW(externalChoice_, BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : configurationInteger(text(fields_[6]));
        candidate.udpPortStart = configurationInteger(text(fields_[7])); candidate.udpPortEnd = configurationInteger(text(fields_[8])); candidate.idleTimeoutSec = configurationInteger(text(fields_[9]));
        candidate.recordDir = trim(text(fields_[10]));
        candidate.recordFolderMode = storageModeDraft(); candidate.recordFolderPattern = text(storagePattern_);
        candidate.trayNotifications = SendMessageW(notificationChoice_, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
        const int close = static_cast<int>(SendMessageW(closeChoice_, CB_GETCURSEL, 0, 0));
        candidate.closeAction = close == 2 ? "exit" : close == 1 ? "minimize" : "tray";
        const int interval = static_cast<int>(SendMessageW(intervalChoice_, CB_GETCURSEL, 0, 0));
        candidate.healthIntervalSec = interval >= 0 && interval < 5 ? intervals_[interval] : g_cfg.healthIntervalSec;
        if (candidate.recordDir != g_cfg.recordDir) candidate.ledgerFile = candidate.recordDir + "\\calls.csv";
        auto invalid = candidate.errors();
        if (candidate.hookPort == 0 && SendMessageW(externalChoice_, BM_GETCHECK, 0, 0) != BST_CHECKED) invalid.push_back("hook_port");
        if (!invalid.empty()) {
            const std::map<string, int> targets = {{"phone_ip", 400}, {"phone_user", 402}, {"phone_pass_dpapi", 403}, {"record_dir", 410}, {"record_folder_mode", 418}, {"record_folder_pattern", 419},
                {"phone_address_mode", 416}, {"phone_mac", 417}, {"phone_port", 401}, {"advertise_ip", 404}, {"control_port", 405},
                {"hook_port", 406}, {"udp_port_range", 407}, {"idle_timeout", 409}, {"health_interval", 412}, {"close_action", 411}, {"tray_notifications", 414}};
            const auto target = targets.find(invalid.front());
            const int id = target == targets.end() ? 400 : target->second;
            switchPage(3); selectSettingsGroup(id == 400 || id == 402 || id == 403 || id == 410 ? 0 : id == 411 || id == 414 || id == 418 || id == 419 ? 2 : 1);
            SetFocus(GetDlgItem(window_, id));
            wstring description = uiText("message.invalid");
            for (const auto& item : invalid) description += L"\n• " + fieldDisplayName(item) + L" [" + utf8ToWide(item) + L"]";
            MessageBoxW(window_, description.c_str(), L"PhoneRecord", MB_OK | MB_ICONWARNING); return;
        }
        if (!saveConfiguration(candidate)) { MessageBoxW(window_, uiText("message.save_failed"), L"PhoneRecord", MB_OK | MB_ICONERROR); return; }
        g_closeAction = close; g_healthIntervalSec = candidate.healthIntervalSec; g_trayNotifications = candidate.trayNotifications != 0;
        restartNeeded_ = candidate.phoneAddressMode != g_cfg.phoneAddressMode || candidate.phoneMac != g_cfg.phoneMac || candidate.phoneIp != g_cfg.phoneIp || candidate.phonePort != g_cfg.phonePort || candidate.phoneUser != g_cfg.phoneUser || candidate.phonePass != g_cfg.phonePass || candidate.advertiseIp != g_cfg.advertiseIp || candidate.controlPort != g_cfg.controlPort || candidate.hookPort != g_cfg.hookPort || candidate.udpPortStart != g_cfg.udpPortStart || candidate.udpPortEnd != g_cfg.udpPortEnd || candidate.idleTimeoutSec != g_cfg.idleTimeoutSec || candidate.recordDir != g_cfg.recordDir || candidate.recordFolderMode != g_cfg.recordFolderMode || candidate.recordFolderPattern != g_cfg.recordFolderPattern;
        MessageBoxW(window_, uiText("message.saved"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); InvalidateRect(window_, NULL, FALSE);
    }

    /// Let the operator choose an existing output directory using a native folder picker.
    void browse() {
        BROWSEINFOW info{}; info.hwndOwner = window_; info.lpszTitle = uiText("dialog.folder"); info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE result = SHBrowseForFolderW(&info);
        if (result) { wchar_t path[MAX_PATH]{}; if (SHGetPathFromIDListW(result, path)) SetWindowTextW(fields_[10], path); CoTaskMemFree(result); }
    }

    /// Open the configured recording or current diagnostic directory on explicit user action.
    void openFolder(bool recordings) {
        wchar_t path[32768]{}; GetFullPathNameW(recordings ? utf8ToWide(g_cfg.recordDir).c_str() : L".", static_cast<DWORD>(std::size(path)), path, NULL);
        ShellExecuteW(window_, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }

#ifdef _WIN64
    /// Navigate to the embedded speech page; retain the main shell and ordinary sidebar selection.
    void openSpeech(const wstring& wav = L"@live") {
        HWND child = SpeechPage::show(window_, wav, dpi_);
        if (!child) return;
        controls_.erase(std::remove_if(controls_.begin(), controls_.end(), [](const Control& item) { return item.id == 960; }), controls_.end());
        const int page=wav==L"@live"?6:8;
        controls_.push_back({child,page,960,""});
        switchPage(page);
    }
#endif

    /// Dispatch user operations without running network requests on the GUI thread.
    void command(int id) {
#ifdef _WIN64
        if(id==107){openSpeech(L"@live");return;}
        if(id==109){openSpeech(L"");return;}
        if (id == 225) {
            const string path = selectedFile();
            if (path.empty()) return;
            wchar_t absolute[32768]{}; GetFullPathNameW(utf8ToWide(path).c_str(), static_cast<DWORD>(std::size(absolute)), absolute, NULL);
            openSpeech(absolute);
            return;
        }
#endif
        if (id == 9900) { capturePreview(); return; }
        if (id >= 100 && id <= 105) { switchPage(id - 100); if (id == 105) refreshUpdates(true); return; }
        if (id == 820 || id == 903) { updates_.request(); refreshUpdates(true); return; }
        if (id == 821) { updates_.request(true); return; }
        if (id == 822) { installUpdate(); return; }
        if (id == 506 || id == 315 || id == 435) { moreActions(id); return; }
        if (id == 830) { updatePreferencesMenu(); return; }
        if (id >= 840 && id <= 845) {
            if (id == 845) SendMessageW(updateBalloons_, BM_SETCHECK, SendMessageW(updateBalloons_, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            else SendMessageW(updateInterval_, CB_SETCURSEL, id - 840, 0);
            configureUpdates(); return;
        }
        if (id == 904) { command(901); switchPage(5); refreshUpdates(true); return; }
        if (id == 825) { configureUpdates(); return; }
        if (id == 502) { requestCompatibility(); return; }
        if (id == 503) { g_commands.enqueue("phone"); return; }
        if (id == 505) { refreshLogs(true); return; }
        if (id == 106) { ShellExecuteW(window_, L"open", L"https://github.com/ChenRanReal/PhoneRecord", NULL, NULL, SW_SHOWNORMAL); return; }
        if (id == 504) { if (phoneAddress().empty()) { MessageBoxW(window_, uiText("address.unresolved"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return; } ShellExecuteW(window_, L"open", utf8ToWide("http://" + phoneAddress() + ":" + std::to_string(g_cfg.phonePort)).c_str(), NULL, NULL, SW_SHOWNORMAL); return; }
        if (id == 216) { const bool ok = player_.playing() ? player_.pause() : player_.play(); if (!ok) showPlayerError(); else updatePlayer(); return; }
        if (id == 217) { if (!player_.stop()) showPlayerError(); else updatePlayer(); return; }
        if (id == 214) { refreshRecordings(true); return; }
        if (id == 221) { renameRecording(); return; }
        if (id == 215) { openFolder(true); return; }
        if (id == 312) { copyReport(); return; }
        if (id == 313) { exportReport(); return; }
        if (id == 314) { openFolder(false); return; }
        if (id == 420) { saveSettings(); return; }
        if (id >= 432 && id <= 434) { selectSettingsGroup(id - 432); return; }
        if (id == 424) { requestPhoneSetup(); return; }
        if (id == 422) { switchPage(4); SendMessageW(helpTopic_, CB_SETCURSEL, 6, 0); refreshHelp(); return; }
        if (id == 423 || id == 804) { requestCompatibility(); return; }
        if (id == 805) { copyReport(true); return; }
        if (id == 414) { showFieldHelp(414); return; }
        if (id == 418 || id == 419) { SetWindowTextW(settingNote_, uiText("storage.help"));
        } else if (id >= 416 && id <= 417) {
            SetWindowTextW(settingNote_, uiText(id == 417 ? "help.field.phone_mac" : "help.field.phone_address_mode"));
        } else if (id == 415) { changeStartup(); return; }
        if (id == 413) { EnableWindow(fields_[6], SendMessageW(externalChoice_, BM_GETCHECK, 0, 0) != BST_CHECKED); showFieldHelp(413); return; }
        if (id == 421) { browse(); return; }
        if (id == 211 || id == 212 || id == 213) {
            const string path = selectedFile(); if (path.empty()) return;
            if (id == 212) { const size_t prefix = g_cfg.recordDir.size() + 1; revealFile(path.substr(prefix)); return; }
            HANDLE check = CreateFileW(utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (check == INVALID_HANDLE_VALUE) { MessageBoxW(window_, uiText("message.busy_file"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return; } CloseHandle(check);
            if (id == 211) {
                if ((path != player_.path() && !player_.open(path, window_)) || !player_.play()) { showPlayerError(); return; }
                const size_t split = path.find_last_of('\\'); const auto title = readRecordingTitle(utf8ToWide(path));
                SetWindowTextW(playerCaption_, title.value.empty() ? utf8ToWide(path.substr(split + 1)).c_str() : title.value.c_str()); updatePlayer();
            }
            if (id == 213) {
                if (analysisWorker_.joinable()) analysisWorker_.join();
                EnableWindow(GetDlgItem(window_, 213), FALSE);
                cancelAnalysis_ = false;
                analysisWorker_ = std::thread([this, path]() { const bool ok = analyseExistingWav(path, &cancelAnalysis_); if (HWND window = g_mainWindow.load()) PostMessageW(window, WM_APP + 23, ok, 0); });
            }
            return;
        }
        if (id == 901) { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); return; }
        if (id == 902) {
            if ((g_activeRecordings > 0 || diagnosticSnapshot().callActive) && MessageBoxW(window_, uiText("message.active_exit"), L"PhoneRecord", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
            DestroyWindow(window_);
        }
    }

    /// Render the actual hidden native controls into an offline-only bitmap for verification.
    void capturePreview() {
        if (!g_offline || g_cfg.phoneIp != "127.0.0.1" || g_uiCaptureDirectory.empty()) return;
        RECT client{}; GetClientRect(window_, &client);
        HDC reference = GetDC(window_), dc = CreateCompatibleDC(reference);
        HBITMAP bitmap = CreateCompatibleBitmap(reference, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(dc, bitmap);
        paint(dc);
        for (const auto& child : controls_) {
            if (child.page >= 0 && child.page != page_) continue;
            if (!(GetWindowLongW(child.window, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT bounds{}; GetWindowRect(child.window, &bounds);
            POINT point{bounds.left, bounds.top}; ScreenToClient(window_, &point);
            const int saved = SaveDC(dc); SetViewportOrgEx(dc, point.x, point.y, NULL);
            IntersectClipRect(dc, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
            SendMessageW(child.window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            wchar_t kind[32]{}; GetClassNameW(child.window, kind, 32);
            if (wcscmp(kind, L"Edit") == 0) {
                // Classic EDIT does not consistently implement WM_PRINT while its
                // parent is hidden. Render its real current value in the preview.
                RECT input{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
                FillRect(dc, &input, white_); HBRUSH edge = CreateSolidBrush(RGB(209, 219, 234)); FrameRect(dc, &input, edge); DeleteObject(edge);
                const int count = GetWindowTextLengthW(child.window); wstring value(static_cast<size_t>(count) + 1, L'\0'); GetWindowTextW(child.window, &value[0], count + 1); value.resize(count);
                const LONG style = GetWindowLongW(child.window, GWL_STYLE);
                if (style & ES_PASSWORD) value.assign(value.size(), L'●');
                const bool cue = value.empty() && (child.id == 210 || child.id == 310);
                if (cue) value = uiText(child.id == 210 ? "search.recordings" : "search.logs");
                InflateRect(&input, -px(7), -px(3)); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, cue ? RGB(126, 140, 161) : RGB(31, 51, 77));
                HGDIOBJ oldFont = SelectObject(dc, font_); DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &input, style & ES_MULTILINE ? DT_LEFT | DT_WORDBREAK : DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); SelectObject(dc, oldFont);
            }
            if (wcscmp(kind, L"ComboBox") == 0) {
                const LRESULT selected = SendMessageW(child.window, CB_GETCURSEL, 0, 0);
                if (selected >= 0) {
                    const LRESULT count = SendMessageW(child.window, CB_GETLBTEXTLEN, selected, 0);
                    if (count >= 0 && count < 2048) {
                        wstring value(static_cast<size_t>(count) + 1, L'\0'); SendMessageW(child.window, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(&value[0]));
                        RECT input{px(7), px(1), bounds.right - bounds.left - px(24), bounds.bottom - bounds.top - px(1)};
                        FillRect(dc, &input, white_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(31, 51, 77));
                        HGDIOBJ oldFont = SelectObject(dc, font_); DrawTextW(dc, value.c_str(), static_cast<int>(count), &input, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); SelectObject(dc, oldFont);
                    }
                }
            }
            RestoreDC(dc, saved);
        }
        SelectObject(dc, previous);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = client.right;
        info.bmiHeader.biHeight = -client.bottom; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        const DWORD length = client.right * client.bottom * 4;
        std::vector<uint8_t> pixels(length);
        if (GetDIBits(reference, bitmap, 0, client.bottom, pixels.data(), &info, DIB_RGB_COLORS)) {
            BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits + length;
            const wstring path = g_uiCaptureDirectory + L"\\PhoneRecord-" + std::to_wstring(page_) + L".bmp";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<char*>(&header), sizeof(header)); output.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(info.bmiHeader)); output.write(reinterpret_cast<char*>(pixels.data()), length);
            LOGI("UI_PREVIEW_RENDERED page=%d synthetic_only=true", page_);
        }
        DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window_, reference);
    }

    /// Persist only update preferences, independently of any unsaved connection draft.
    void configureUpdates() {
        const int choices[] = {0, 1, 6, 12, 24};
        const LRESULT selected = SendMessageW(updateInterval_, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected >= 5) return;
        if (!updates_.configure(choices[selected], SendMessageW(updateBalloons_, BM_GETCHECK, 0, 0) == BST_CHECKED)) {
            const auto preferences = updates_.preferences();
            for (int index = 0; index < 5; ++index) if (choices[index] == preferences.hours) SendMessageW(updateInterval_, CB_SETCURSEL, index, 0);
            SendMessageW(updateBalloons_, BM_SETCHECK, preferences.balloons ? BST_CHECKED : BST_UNCHECKED, 0);
            MessageBoxW(window_, uiText("update.preference_error"), L"PhoneRecord", MB_OK | MB_ICONERROR);
        }
        refreshUpdates(true);
    }

    /// Create an owned small icon with a solid arrow badge; alpha remains readable on either theme.
    HICON createUpdateIcon() {
        const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        HDC dc = CreateCompatibleDC(NULL); DWORD* pixels = NULL;
        HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), NULL, 0);
        HBITMAP mask = CreateBitmap(width, height, 1, 1, NULL);
        if (!dc || !color || !mask || !pixels) { if (color) DeleteObject(color); if (mask) DeleteObject(mask); if (dc) DeleteDC(dc); return NULL; }
        HGDIOBJ previous = SelectObject(dc, color); ZeroMemory(pixels, width * height * sizeof(DWORD));
        DrawIconEx(dc, 0, 0, applicationIcon(true), width, height, 0, NULL, DI_NORMAL);
        const int badge = std::max(9, width * 2 / 3), left = width - badge, top = height - badge, center = badge / 2;
        for (int y = 0; y < badge; ++y) for (int x = 0; x < badge; ++x) {
            const int dx = x - center, dy = y - center;
            if (dx * dx + dy * dy > center * center) continue;
            const bool arrow = (y >= badge / 4 && y <= badge / 2 && abs(dx) <= y - badge / 4) || (abs(dx) <= std::max(1, badge / 10) && y >= badge / 3 && y < badge * 4 / 5);
            pixels[(top + y) * width + left + x] = arrow ? 0xffffffff : 0xff087fbd;
        }
        SelectObject(dc, previous);
        ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = color; icon.hbmMask = mask;
        HICON result = CreateIconIndirect(&icon); DeleteObject(color); DeleteObject(mask); DeleteDC(dc); return result;
    }

    /// Refresh update controls and persistent tray availability; never performs a network request.
    void refreshUpdates(bool force = false) {
        const auto state = updates_.state(); const auto preferences = updates_.preferences();
        const bool available = !state.manifest.version.empty();
        if (force || state.revision != updateRevision_) {
            updateRevision_ = state.revision;
            wstring status = wstring(uiText("update.current")) + L" " + utf8ToWide(kProductVersion);
            if (available) status += L"\r\n" + wstring(uiText("update.new_version")) + L" " + utf8ToWide(state.manifest.version);
            status += L"\r\n" + wstring(uiText(state.status.c_str()));
            if (state.status == "update.downloading") status += L" " + std::to_wstring(state.percent) + L"%";
            if (!state.checkedAt.empty()) status += L"\r\n" + wstring(uiText("update.checked_at")) + L" " + utf8ToWide(state.checkedAt);
            SetWindowTextW(updateStatus_, status.c_str());
            const wstring notes = wstring(uiText("update.notes_language")) + L"\r\n\r\n" + (state.notes.empty() ? uiText("update.no_notes") : utf8ToWide(state.notes));
            if (text(updateNotes_) != wideToUtf8(notes)) SetWindowTextW(updateNotes_, notes.c_str());
            EnableWindow(GetDlgItem(window_, 820), !state.busy && !g_offline);
            EnableWindow(GetDlgItem(window_, 821), available && !state.busy && state.staged.empty() && !g_offline);
            SetWindowTextW(GetDlgItem(window_, 105), uiText(available ? "update.nav_available" : "nav.updates"));
            if (available && !updateIcon_) updateIcon_ = createUpdateIcon();
            tray_.hIcon = available && updateIcon_ ? updateIcon_ : applicationIcon(true);
            const wstring tip = L"PhoneRecord " + utf8ToWide(kProductVersion) + (available ? L" — " + wstring(uiText("update.available")) + L" " + utf8ToWide(state.manifest.version) : L"");
            wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE); tray_.uFlags = (tray_.uFlags & NIF_GUID) | NIF_ICON | NIF_MESSAGE | NIF_TIP;
            if (trayAdded_) Shell_NotifyIconW(NIM_MODIFY, &tray_);
            RedrawWindow(GetDlgItem(window_, 105), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        refreshActionVisibility();
        const auto recording = diagnosticSnapshot();
        EnableWindow(GetDlgItem(window_, 822), !state.staged.empty() && !state.busy && !recording.callActive && g_sessionCount == 0 && g_activeRecordings == 0 && !recording.busy && !analysisWorker_.joinable() && !g_offline);
#ifdef _WIN64
        const auto speech = g_speech.state();
        if (speech.busy || speech.waiting) EnableWindow(GetDlgItem(window_, 822), FALSE);
#endif
        if (available && !state.busy && !g_offline && preferences.balloons && preferences.notified != state.manifest.version &&
            lastUpdateBalloon_ != state.manifest.version && !recording.callActive && g_activeRecordings == 0 && trayAdded_ && !trayBalloonActive_ && GetTickCount64() >= nextBalloon_) {
            NOTIFYICONDATAW info = tray_; info.uFlags = (tray_.uFlags & NIF_GUID) | NIF_INFO; info.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME | NIIF_NOSOUND;
            const wstring title = wstring(uiText("update.available")) + L" " + utf8ToWide(state.manifest.version);
            wcsncpy_s(info.szInfoTitle, title.c_str(), _TRUNCATE); wcsncpy_s(info.szInfo, uiText("update.balloon_body"), _TRUNCATE);
            nextBalloon_ = GetTickCount64() + 60000;
            if (Shell_NotifyIconW(NIM_MODIFY, &info)) {
                trayBalloonActive_ = updateBalloonActive_ = true; lastUpdateBalloon_ = state.manifest.version;
                if (!updates_.markNotified(state.manifest.version)) LOGW("UPDATE_NOTIFICATION_PREFERENCE_SAVE_FAILED");
                LOGI("UPDATE_BALLOON_REQUESTED version=%s", state.manifest.version.c_str());
            }
        }
    }

    /// Confirm an explicit service interruption, then atomically refuse installation if a call won the race.
    void installUpdate() {
#ifdef _WIN64
        const auto speech = g_speech.state();
        if (speech.busy || speech.waiting) { MessageBoxW(window_, uiText("speech.update_busy"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return; }
#endif
        const auto state = updates_.state();
        if (state.busy || state.staged.empty() || g_offline) return;
        if (MessageBoxW(window_, uiText("update.confirm_install"), L"PhoneRecord", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        if (!updates_.reserveInstall(state)) { MessageBoxW(window_, uiText("update.prepare_failed"), L"PhoneRecord", MB_OK | MB_ICONWARNING); return; }
        bool busy = false;
        {
            std::lock_guard<std::mutex> admission(g_updateAdmission);
            const auto recording = diagnosticSnapshot();
            busy = recording.callActive || recording.busy || g_sessionCount > 0 || g_activeRecordings > 0 || g_pendingCallEvents > 0 || analysisWorker_.joinable();
#ifdef _WIN64
            const auto currentSpeech = g_speech.state(); busy = busy || currentSpeech.busy || currentSpeech.waiting > 0;
#endif
            if (!busy) g_updateInstalling = true;
        }
        if (busy) { updates_.cancelInstall(); MessageBoxW(window_, uiText("update.call_busy"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION); return; }
        if (!updateBeginInstall(state, g_configPath)) {
            { std::lock_guard<std::mutex> admission(g_updateAdmission); g_updateInstalling = false; }
            updates_.cancelInstall();
            MessageBoxW(window_, uiText("update.prepare_failed"), L"PhoneRecord", MB_OK | MB_ICONERROR); return;
        }
        LOGI("UPDATE_INSTALL_CONFIRMED version=%s", state.manifest.version.c_str());
        updates_.preserveStaging();
        DestroyWindow(window_);
    }

    /// Show a bounded native tray notification without caller identifiers.
    void balloon(const wchar_t* message) {
        if (!g_trayNotifications || g_offline) { LOGD("TRAY_NOTIFICATION_SUPPRESSED preference=%d offline=%d", g_trayNotifications.load(), g_offline); return; }
        if (!trayAdded_) return;
        if (trayBalloonActive_) return;
        NOTIFYICONDATAW info = tray_; info.uFlags = (tray_.uFlags & NIF_GUID) | NIF_INFO; info.dwInfoFlags = NIIF_INFO;
        wcscpy_s(info.szInfoTitle, L"PhoneRecord"); wcsncpy_s(info.szInfo, message, _TRUNCATE); const bool sent = Shell_NotifyIconW(NIM_MODIFY, &info) != FALSE; if (sent) { trayBalloonActive_ = true; updateBalloonActive_ = false; } LOGD("TRAY_NOTIFICATION_REQUESTED sent=%d", sent);
    }

    /// Native message handler; background tasks communicate only through PostMessageW.
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            tray_.uFlags = (tray_.uFlags & NIF_GUID) | NIF_ICON | NIF_MESSAGE | NIF_TIP; trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
            refreshUpdates(true); return 0;
        }
        switch (message) {
        case WM_CREATE: create(); return 0;
        case WM_ACTIVATE: if (LOWORD(wparam) != WA_INACTIVE) refreshStartup(); break;
        case WM_SIZE: if (font_) layout(); return 0;
        case WM_GETMINMAXINFO: { auto* limits = reinterpret_cast<MINMAXINFO*>(lparam); limits->ptMinTrackSize.x = px(980); limits->ptMinTrackSize.y = px(760); return 0; }
        case WM_DPICHANGED: { dpi_ = HIWORD(wparam); destroyFonts(); createFonts(); for (const auto& child : controls_) SendMessageW(child.window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); const RECT* area = reinterpret_cast<RECT*>(lparam); SetWindowPos(window_, NULL, area->left, area->top, area->right - area->left, area->bottom - area->top, SWP_NOZORDER | SWP_NOACTIVATE); layout(); return 0; }
        case WM_SETCURSOR:
            if (reinterpret_cast<HWND>(wparam) == GetDlgItem(window_, 106)) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_DRAWITEM: drawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_CTLCOLORSTATIC: SetBkColor(reinterpret_cast<HDC>(wparam), RGB(246, 248, 252)); SetTextColor(reinterpret_cast<HDC>(wparam), RGB(68, 88, 114)); return reinterpret_cast<LRESULT>(background_);
        case WM_CTLCOLOREDIT: SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255)); SetTextColor(reinterpret_cast<HDC>(wparam), RGB(31, 51, 77)); return reinterpret_cast<LRESULT>(white_);
        case WM_COMMAND:
            if (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 416) { refreshAddressMode(); return 0; }
            if ((HIWORD(wparam) == EN_SETFOCUS && LOWORD(wparam) == 417) || (HIWORD(wparam) == CBN_SETFOCUS && LOWORD(wparam) == 416)) { showFieldHelp(LOWORD(wparam)); return 0; }
            if (HIWORD(wparam) == EN_KILLFOCUS && LOWORD(wparam) == 400) { requestLocalIpSuggestion(); return 0; }
            if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == 404) { if (!changingLocalIp_) suggestedLocalIp_.clear(); return 0; }
            if (HIWORD(wparam) == BN_SETFOCUS && LOWORD(wparam) == 415) { showFieldHelp(415); return 0; }
            if (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 430) { changeLanguage(); return 0; }
            if (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 824) { configureUpdates(); return 0; }
            if (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 418) { refreshSettingsVisibility(); showFieldHelp(418); return 0; }
            if (HIWORD(wparam) == EN_CHANGE && (LOWORD(wparam) == 419 || LOWORD(wparam) == 410)) { refreshStoragePreview(); return 0; }
            if ((HIWORD(wparam) == EN_SETFOCUS && LOWORD(wparam) == 419) || (HIWORD(wparam) == CBN_SETFOCUS && LOWORD(wparam) == 418)) { showFieldHelp(LOWORD(wparam)); return 0; }
            if (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 800) { refreshHelp(); return 0; }
            if (HIWORD(wparam) == EN_SETFOCUS && LOWORD(wparam) >= 400 && LOWORD(wparam) <= 410) { showFieldHelp(LOWORD(wparam)); return 0; }
            if (HIWORD(wparam) == CBN_SETFOCUS && (LOWORD(wparam) == 411 || LOWORD(wparam) == 412)) { showFieldHelp(LOWORD(wparam)); return 0; }
            if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == 210) { refreshRecordings(false); return 0; }
            if ((HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == 310) || (HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == 311)) { refreshLogs(true); return 0; }
            if (HIWORD(wparam) == BN_CLICKED || LOWORD(wparam) >= 900) command(LOWORD(wparam));
            return 0;
        case WM_NOTIFY: {
            auto* notice = reinterpret_cast<NMHDR*>(lparam);
            if (notice->idFrom == 610 && notice->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lparam)->wVKey == VK_F2) { PostMessageW(window_, WM_COMMAND, 221, 0); return 0; }
            if (notice->idFrom == 610 && notice->code == NM_DBLCLK) command(211);
            if (notice->idFrom == 600 && notice->code == NM_DBLCLK) { switchPage(2); SendMessageW(logFilter_, CB_SETCURSEL, 1, 0); refreshLogs(true); }
            if (notice->idFrom == 620 && notice->code == LVN_ITEMCHANGED) {
                const int selected = ListView_GetNextItem(diagnostics_, -1, LVNI_SELECTED);
                if (selected >= 0 && selected < static_cast<int>(visibleLogs_.size())) {
                    const auto& event = visibleLogs_[selected];
                    const string code = event.text.substr(0, event.text.find(' '));
                    const wstring value = utf8ToWide(event.time + "  [" + event.level + "]\r\n" + event.text) + L"\r\n\r\n" + diagnosticHint(code);
                    SetWindowTextW(detail_, value.c_str());
                }
            }
            return 0;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == recordings_) { recordingContextMenu(lparam); return 0; }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lparam) == playerSeek_) {
                const int code = LOWORD(wparam);
                seeking_ = code == TB_THUMBTRACK;
                if (code != TB_THUMBTRACK) { const LRESULT position = SendMessageW(playerSeek_, TBM_GETPOS, 0, 0); if (!player_.seek(static_cast<DWORD>(static_cast<uint64_t>(player_.length()) * position / 1000), true)) showPlayerError(); else updatePlayer(); }
                return 0;
            }
            break;
        case WM_TIMER: {
            // Modal editors run their own message loop; keep the documented stop signal responsive.
            if (stopSignal_ && WaitForSingleObject(stopSignal_, 0) == WAIT_OBJECT_0) {
                ResetEvent(stopSignal_);
                if (renameDialog_) EndDialog(renameDialog_, IDCANCEL);
                PostMessageW(window_, WM_COMMAND, 902, 0); return 0;
            }
            #ifdef _WIN64
            const auto liveSpeech = g_speech.state();
            if (liveSpeech.liveEnabled && liveSpeech.recording && liveSpeech.liveWav != lastLiveSpeech_) { lastLiveSpeech_ = liveSpeech.liveWav; openSpeech(L"@live"); }
#endif
            const auto state = diagnosticSnapshot();
            const int sessions = static_cast<int>(g_sessionCount.load()), recordings = g_activeRecordings.load();
            if (state.changedAt != lastChanged_ || sessions != lastSessions_ || recordings != lastActive_) { lastChanged_ = state.changedAt; lastSessions_ = sessions; lastActive_ = recordings; InvalidateRect(window_, NULL, FALSE); }
            const auto inspection = compatibilitySnapshot();
            EnableWindow(GetDlgItem(window_, 804), !state.busy && !state.callActive && sessions == 0 && inspection.verdict != "running");
            EnableWindow(GetDlgItem(window_, 423), !state.busy && !state.callActive && sessions == 0 && inspection.verdict != "running");
            EnableWindow(GetDlgItem(window_, 502), !state.busy && !state.callActive && sessions == 0 && inspection.verdict != "running");
            EnableWindow(GetDlgItem(window_, 805), inspection.verdict != "idle" && inspection.verdict != "running");
            if (page_ == 4 && SendMessageW(helpTopic_, CB_GETCURSEL, 0, 0) == 5 && (inspection.revision != lastCompatRevision_ || compatibilityRuntimeVerified(inspection, state, sessions) != lastCompatRuntime_)) refreshHelp();
            if (lastAddressRevision_ != g_phoneAddressRevision.load()) refreshAddressStatus();
            updatePlayer(); refreshLogs(false); refreshUpdates(); refreshPhoneSetup();
            if (state.files != lastFiles_) { lastFiles_ = state.files; if (page_ == 1) refreshRecordings(true); }
            return 0;
        }
        case kMediaNotification: balloon(uiText("message.media")); InvalidateRect(window_, NULL, FALSE); return 0;
        case kFileNotification: if (page_ == 1) refreshRecordings(true); InvalidateRect(window_, NULL, FALSE); return 0;
        case WM_APP + 23: if (analysisWorker_.joinable()) analysisWorker_.join(); EnableWindow(GetDlgItem(window_, 213), TRUE); refreshRecordings(true); if (!wparam) MessageBoxW(window_, uiText("message.analyse_failed"), L"PhoneRecord", MB_OK | MB_ICONWARNING); return 0;
        case WM_APP + 24: refreshUpdates(); return 0;
        case WM_APP + 25: completeLocalIpSuggestion(); return 0;
        case WM_APP + 31:
            if (LOWORD(lparam) == NIN_BALLOONUSERCLICK) { if (updateBalloonActive_) command(904); trayBalloonActive_ = updateBalloonActive_ = false; return 0; }
            if (LOWORD(lparam) == NIN_BALLOONTIMEOUT || LOWORD(lparam) == NIN_BALLOONHIDE) trayBalloonActive_ = updateBalloonActive_ = false;
            if (LOWORD(lparam) == WM_LBUTTONDBLCLK) command(901);
            else if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 901, uiText("tray.show")); AppendMenuW(menu, MF_SEPARATOR, 0, NULL); AppendMenuW(menu, MF_STRING, 902, uiText("tray.exit"));
                const auto update = updates_.state();
                InsertMenuW(menu, 1, MF_BYPOSITION | MF_STRING, 904, uiText(update.manifest.version.empty() ? "nav.updates" : update.staged.empty() ? "update.nav_available" : "update.ready"));
                InsertMenuW(menu, 2, MF_BYPOSITION | MF_STRING | (update.busy || g_offline ? MF_GRAYED : 0), 903, uiText("update.check"));
                POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window_); TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, NULL); DestroyMenu(menu);
            }
            return 0;
        case WM_CLOSE:
            if (g_closeAction == 2 || (!trayAdded_ && g_closeAction == 0)) command(902);
            else if (g_closeAction == 1) ShowWindow(window_, SW_MINIMIZE);
            else { ShowWindow(window_, SW_HIDE); balloon(uiText("message.minimized")); }
            return 0;
        case WM_DESTROY:
            if (localIpWorker_.joinable()) localIpWorker_.join();
            updates_.stop();
            player_.close();
            g_mainWindow = NULL; KillTimer(window_, 1);
            if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
            cancelAnalysis_ = true;
            if (analysisWorker_.joinable()) analysisWorker_.join();
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    /// Native owner window.
    HWND window_ = NULL;
    /// Effective DPI and selected page.
    int dpi_ = 96, page_ = 0;
    /// GDI font hierarchy owned by this view.
    HFONT font_ = NULL, smallFont_ = NULL, titleFont_ = NULL, valueFont_ = NULL;
    /// Cached background brushes.
    HBRUSH background_ = NULL, white_ = NULL;
    /// Owned child control registry.
    std::vector<Control> controls_;
    /// Recording, log and recent-event lists.
    HWND recordings_ = NULL, diagnostics_ = NULL, recent_ = NULL;
    /// Search, filter and diagnostic detail controls.
    HWND recordingSearch_ = NULL, logSearch_ = NULL, logFilter_ = NULL, detail_ = NULL;
    /// Non-owned process stop event, valid until this view has been destroyed.
    HANDLE stopSignal_ = NULL;
    /// Current owned title editor, canceled before a requested application shutdown.
    HWND renameDialog_ = NULL;
    /// Owned internal playback device and cursor controls.
    WavPlayer player_;
    HWND playerSeek_ = NULL, playerCaption_ = NULL, playerTime_ = NULL, followRecent_ = NULL;
    bool seeking_ = false;
    /// Persisted user-interface behaviour and idle-only check frequency.
    HWND closeLabel_ = NULL, closeChoice_ = NULL, intervalLabel_ = NULL, intervalChoice_ = NULL;
    const int intervals_[5] = {0, 30, 60, 120, 300};
    /// Locale, trigger mode and embedded reference controls, all owned by the UI thread.
    HWND languageChoice_ = NULL, externalChoice_ = NULL, helpTopic_ = NULL, helpText_ = NULL, notificationChoice_ = NULL;
    /// Immediate current-user login preference, read from Windows rather than config.ini.
    HWND startupChoice_ = NULL;
    /// Avoid resetting report scroll position when unrelated log or RTP counters change.
    uint64_t lastCompatRevision_ = 0;
    bool lastCompatRuntime_ = false;
    int focusedSetting_ = -1;
    /// Column keys allow headers to change language while preserving selected rows and search drafts.
    std::map<HWND, std::vector<string>> columnKeys_;
    /// Settings inputs and associated resource-backed labels.
    /// Connection-mode controls preserve the selected mode and both identity drafts independently.
    unsigned lastAddressRevision_ = 0;
    HWND addressStatus_ = NULL;
    HWND addressModeLabel_ = NULL, addressModeChoice_ = NULL, addressIdentityLabel_ = NULL, addressMac_ = NULL;
    HWND fields_[11]{}, fieldLabels_[11]{}, settingNote_ = NULL;
    /// View-only category and read-only local-address summary; underlying edit controls retain their values.
    int settingsGroup_ = 0;
    HWND basicIpSummary_ = NULL;
    /// Storage policy drafts and side-effect-free destination preview.
    HWND storageMode_ = NULL, storagePattern_ = NULL, storagePreview_ = NULL;
    /// Stable recording snapshots for row actions and filtering.
    std::vector<CallItem> calls_, visibleCalls_;
    /// Bounded visible log snapshot, keeping the selected sequence stable.
    std::vector<DiagnosticEntry> visibleLogs_;
    /// Stable recent-event identities preserve the reading position when following is paused.
    std::vector<uint64_t> recentSequences_;
    /// Change markers prevent unnecessary rescans and log rebuilding.
    uint64_t lastLog_ = 0, lastFiles_ = 0;
    /// Last state update rendered in the overview.
    string lastChanged_;
    /// Session cleanup can finish after the last diagnostic update; repaint its idle transition too.
    int lastSessions_ = -1, lastActive_ = -1;
    /// Settings were saved for the next process start, not applied to live sockets.
    bool restartNeeded_ = false;
    /// Local-address lookup owns no sockets and writes its result only before the GUI joins it.
    std::thread localIpWorker_;
    string localIpQuery_, localIpResult_, suggestedLocalIp_;
    /// Distinguish a programmatic suggestion from an operator's manual edit notification.
    bool changingLocalIp_ = false;
    /// Completed setup generation and modal reentry guard, owned by the GUI thread.
    uint64_t setupRevision_ = 0;
    bool setupDialog_ = false;
    /// Owned explicit file-analysis worker.
    std::thread analysisWorker_;
    /// Cancel explicit file analysis before joining on window destruction.
    std::atomic<bool> cancelAnalysis_{false};
    /// Native tray icon data and registration status.
    NOTIFYICONDATAW tray_{};
    /// Whether shell notification registration succeeded.
    bool trayAdded_ = false;
    /// Update worker and page controls are owned for the complete window lifetime.
    UpdateService updates_;
#ifdef _WIN64
    /// Avoid reopening the live page after the user navigates away during the same call.
    wstring lastLiveSpeech_;
#endif
    HWND updateInterval_ = NULL, updateBalloons_ = NULL, updateStatus_ = NULL, updateNotes_ = NULL;
    /// Shell recreation, snapshot invalidation and update-balloon routing.
    UINT taskbarCreated_ = 0;
    uint64_t updateRevision_ = static_cast<uint64_t>(-1), nextBalloon_ = 0;
    bool updateBalloonActive_ = false;
    bool trayBalloonActive_ = false;
    string lastUpdateBalloon_;
    /// Owned overlay icon, independent of the shared application icon.
    HICON updateIcon_ = NULL;
};
