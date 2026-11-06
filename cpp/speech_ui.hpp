#pragma once
#ifdef _WIN64

/// Embedded native speech page. Navigation, colors and lifetime belong to the main desktop shell.
class SpeechPage {
public:
    /// Show settings or a selected transcript inside the existing main window; never create a top-level window.
    static HWND show(HWND parent, const wstring& wav, int dpi) {
        if (instance()) {
            auto* existing = reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(instance(), GWLP_USERDATA));
            if (existing && ((existing->live_ && wav == L"@live") || (!existing->live_ && wav != L"@live" && existing->wav_ == wav))) { existing->refresh(true); return instance(); }
            DestroyWindow(instance());
        }
        auto* self = new SpeechPage; self->live_ = wav == L"@live"; self->wav_ = wav; self->dpi_ = dpi;
        WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.hInstance = GetModuleHandleW(NULL); type.lpfnWndProc = procedure;
        type.lpszClassName = L"PhoneRecordSpeechPage"; type.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassExW(&type);
        HWND result = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 1, 1, parent, reinterpret_cast<HMENU>(960), type.hInstance, self);
        if (!result) { delete self; return NULL; }
        instance() = result; return result;
    }
    /// Child handle is registered as page 6 with the main shell's ordinary control ownership.
    static HWND& instance() { static HWND window = NULL; return window; }
    /// Reflow in the main content area, including DPI changes without resetting preferences or text.
    static void place(int x, int y, int width, int height, int dpi) {
        if (!instance()) return;
        auto* self = reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(instance(), GWLP_USERDATA));
        if (!self) return;
        if (self->dpi_ != dpi) { self->dpi_ = dpi; self->fonts(); }
        MoveWindow(instance(), self->px(x), self->px(y), self->px(width), self->px(height), TRUE);
        self->layout();
    }
private:
    /// Translate the shell's logical design grid into native device pixels.
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    /// Current client extent in logical pixels.
    int width() const { RECT r{}; GetClientRect(window_, &r); return MulDiv(r.right, 96, dpi_); }
    /// Current client height in logical pixels.
    int height() const { RECT r{}; GetClientRect(window_, &r); return MulDiv(r.bottom, 96, dpi_); }
    /// Recreate page-owned fonts when the main window moves to another DPI.
    void fonts() {
        if (font_) DeleteObject(font_); if (small_) DeleteObject(small_);
        font_ = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        small_ = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL { SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font_));
    }
    /// Create an accessible native child; the page owns positions and local command identifiers.
    HWND control(const wchar_t* type, const wchar_t* caption, DWORD style, int id) {
        HWND child = CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(NULL), NULL);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return child;
    }
    /// Position one child using the shell's current DPI.
    void move(int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(window_, id), px(x), px(y), px(w), px(h), TRUE); }
    /// Lay out settings and progress within a normal page at the existing minimum desktop size.
    void layout() {
        const int w = width(), h = height(), field = std::min(460, w - 292);
        if (!viewer_) {
            move(1, 32, 22, w - 64, 30);
            move(40, 32, 74, 210, 22); move(8, 260, 68, field, 180);
            move(42, 32, 118, 210, 22); move(2, 260, 112, field, 200);
            move(43, 32, 162, 210, 22); move(3, 260, 156, field, 140);
            move(7, 32, 239, w - 64, 28);
            move(10, 32, 274, w - 64, 108);
            move(9, w - 264, 474, 116, 30); move(11, w - 136, 474, 104, 30);
            move(4, 16, h - 48, 310, 36); move(5, 338, h - 48, 180, 36);
        } else {
            move(34,w - 292,0,276,160);
            move(30, 24, 64, w - 48, std::max(80, h - 132));
            move(31, 16, h - 48, 166, 36); move(32, 194, h - 48, 166, 36); move(33, 372, h - 48, std::min(280, w - 388), 36);
        }
        InvalidateRect(window_, NULL, FALSE);
    }
    /// Draw one localized text block using the same palette as the surrounding desktop page.
    void text(HDC dc, const wstring& value, int x, int y, int w, int h, COLORREF color, HFONT font, UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) {
        RECT r{px(x), px(y), px(x + w), px(y + h)}; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
        HGDIOBJ old = SelectObject(dc, font); DrawTextW(dc, value.c_str(), -1, &r, flags); SelectObject(dc, old);
    }
    /// Paint a white card using the desktop's border and corner treatment.
    void surface(HDC dc, int x, int y, int w, int h) {
        HPEN edge = CreatePen(PS_SOLID, 1, RGB(209,219,234));
        HGDIOBJ b = SelectObject(dc, white_), p = SelectObject(dc, edge);
        RoundRect(dc, px(x), px(y), px(x + w), px(y + h), px(10), px(10)); SelectObject(dc, b); SelectObject(dc, p); DeleteObject(edge);
    }
    /// Adapt persisted UTF-8 line endings for the native Windows multiline editor and clipboard.
    wstring transcriptText() const {
        string normalized; normalized.reserve(shownText_.size());
        for (size_t i = 0; i < shownText_.size(); ++i) {
            if (shownText_[i] == '\r') { if (i + 1 < shownText_.size() && shownText_[i + 1] == '\n') ++i; normalized += "\r\n"; }
            else if (shownText_[i] == '\n') normalized += "\r\n";
            else normalized += shownText_[i];
        }
        return utf8ToWide(normalized);
    }
    /// Format sanitized failure facts for the inline details panel and explicit copy action.
    wstring failureText(const SpeechState& state) const {
        wstring report = uiText("speech.failure_title");
        const auto field = [&](const char* label, const wstring& value) { if (!value.empty()) report += L"\r\n" + wstring(uiText(label)) + L": " + value; };
        field("speech.failure_stage",uiText(("speech." + (state.failedStep.empty() ? "unknown_step" : state.failedStep)).c_str()));
        field("speech.failure_reason",uiText(("speech." + state.failureCode).c_str()));
        if (!state.operation.empty()) field("speech.failure_operation",uiText(("speech.operation." + state.operation).c_str()));
        field("speech.failure_resource",utf8ToWide(state.resource)); field("speech.failure_origin",utf8ToWide(state.origin));
        field("speech.failure_http",utf8ToWide(state.httpStatus)); field("speech.failure_code",utf8ToWide(state.nativeCode)); field("speech.failure_exit",utf8ToWide(state.processExit));
        field("speech.failure_detail",state.detail.empty() ? uiText("speech.no_failure_detail") : utf8ToWide(state.detail));
        return report;
    }
    /// Copy only on explicit user action, preserving native clipboard ownership on failure.
    void copyText(const wstring& value) {
        if (!OpenClipboard(window_)) return;
        EmptyClipboard(); HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
        if (data) { void* p = GlobalLock(data); if (p) { memcpy(p,value.c_str(),(value.size()+1)*sizeof(wchar_t)); GlobalUnlock(data); if (!SetClipboardData(CF_UNICODETEXT,data)) GlobalFree(data); } else GlobalFree(data); }
        CloseClipboard();
    }
    /// Create either settings or text controls once; navigation never cancels background work.
    void create() {
        fonts(); white_ = CreateSolidBrush(RGB(255,255,255));
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) ramGiB_ = static_cast<unsigned>((memory.ullTotalPhys + (1ULL<<29)) >> 30);
        SYSTEM_INFO system{}; GetNativeSystemInfo(&system); cpuThreads_ = system.dwNumberOfProcessors;
        if (!wav_.empty()) {
            if (live_) wav_ = g_speech.state().liveWav;
            viewer_ = control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 30);
            SendMessageW(viewer_, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);
            control(L"BUTTON", uiText("speech.copy"), WS_TABSTOP | BS_OWNERDRAW, 31);
            control(L"BUTTON", uiText("speech.retry"), WS_TABSTOP | BS_OWNERDRAW, 32);
            control(L"BUTTON", uiText("speech.settings"), WS_TABSTOP | BS_OWNERDRAW, 33);
            HWND version = control(WC_COMBOBOXW,L"",WS_TABSTOP | CBS_DROPDOWNLIST,34);
            SendMessageW(version,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"SenseVoice · INT8"));
            SendMessageW(version,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Qwen3-ASR · 0.6B"));
            SendMessageW(version,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Qwen3-ASR · 1.7B"));
            SendMessageW(version,CB_SETCURSEL,0,0); EnableWindow(version,!live_);
            shownText_ = SpeechService::read(resultPath(L".txt"));
            SetWindowTextW(viewer_, shownText_.empty() ? uiText("speech.no_text") : transcriptText().c_str());
        } else {
            enabled_ = control(L"BUTTON", uiText("speech.enable"), WS_TABSTOP | BS_AUTOCHECKBOX, 1);
            control(L"STATIC", uiText("speech.provider"), 0, 40);
            HWND provider = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 8);
            SendMessageW(provider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SenseVoiceSmall (INT8)")); SendMessageW(provider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Qwen3-ASR 0.6B (CPU)")); SendMessageW(provider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Qwen3-ASR 1.7B (CPU)")); SendMessageW(provider, CB_SETCURSEL, g_speech.model() == "qwen17" ? 2 : g_speech.model() == "qwen" ? 1 : 0, 0);
            control(L"STATIC", uiText("speech.language"), 0, 42);
            language_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 2);
            for (int i = 0; i < 6; ++i) { SendMessageW(language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText((string("speech.lang.") + languages_[i]).c_str()))); if (g_speech.language() == languages_[i]) SendMessageW(language_, CB_SETCURSEL, i, 0); }
            control(L"STATIC", uiText("speech.source"), 0, 43);
            source_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, 3);
            for (const wchar_t* value : {L"Hugging Face", L"HF-Mirror"}) SendMessageW(source_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
            SendMessageW(source_, CB_SETCURSEL, g_speech.source() == "hf-mirror.com" ? 1 : 0, 0);
            control(L"BUTTON", uiText("speech.steps"), WS_TABSTOP | BS_OWNERDRAW, 7);
            control(L"BUTTON", uiText("speech.failure_details"), WS_TABSTOP | BS_OWNERDRAW, 9);
            detail_ = control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 10);
            control(L"BUTTON", uiText("speech.copy_diagnostics"), WS_TABSTOP | BS_OWNERDRAW, 11);
            SendMessageW(detail_,EM_SETLIMITTEXT,16384,0);
            install_ = control(L"BUTTON", uiText("speech.install"), WS_TABSTOP | BS_OWNERDRAW, 4);
            cancel_ = control(L"BUTTON", uiText("speech.cancel"), WS_TABSTOP | BS_OWNERDRAW, 5);
            SendMessageW(enabled_, BM_SETCHECK, g_speech.state().enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        SetTimer(window_, 1, 200, NULL); refresh(true); layout();
    }
    /// Model versions have independent sidecars; selecting one never overwrites the other.
    wstring resultPath(const wchar_t* extension) const { return wav_ + SpeechService::resultSuffix(resultModel_) + extension; }
    /// Save speech-only preferences immediately; ordinary connection settings remain independent.
    void configure() {
        const int lang = static_cast<int>(SendMessageW(language_, CB_GETCURSEL, 0, 0)); if (lang < 0 || lang >= 6) return;
        if (!g_speech.configure(SendMessageW(enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED, languages_[lang], SendMessageW(source_, CB_GETCURSEL, 0, 0) == 1 ? "hf-mirror.com" : "huggingface.co")) {
            MessageBoxW(window_, uiText("speech.save_failed"), uiText("speech.title"), MB_OK | MB_ICONERROR);
            SendMessageW(enabled_, BM_SETCHECK, g_speech.state().enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        refresh(true);
    }
    /// Refresh translated controls and changed transcript bytes without resetting reading position.
    void refresh(bool force = false) {
        const int locale = g_uiLanguage.load();
        if (locale != locale_) {
            locale_ = locale; force = true;
            const std::map<int, const char*> labels = {{1,"speech.enable"},{4,"speech.install"},{5,"speech.cancel"},{7,"speech.steps"},{9,"speech.failure_details"},{11,"speech.copy_diagnostics"},{31,"speech.copy"},{32,"speech.retry"},{33,"speech.settings"},{40,"speech.provider"},{42,"speech.language"},{43,"speech.source"}};
            for (const auto& item : labels) if (HWND child = GetDlgItem(window_, item.first)) SetWindowTextW(child, uiText(item.second));
            if (language_) {
                const LRESULT selected = SendMessageW(language_, CB_GETCURSEL, 0, 0); SendMessageW(language_, CB_RESETCONTENT, 0, 0);
                for (const char* hint : languages_) SendMessageW(language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(uiText((string("speech.lang.") + hint).c_str())));
                SendMessageW(language_, CB_SETCURSEL, selected, 0);
            }
        }
        const auto state = g_speech.state();
        if (live_ && wav_ != state.liveWav) { wav_ = state.liveWav; transcriptStamp_ = 0; force = true; }
        EnableWindow(GetDlgItem(window_,32),!live_ && !wav_.empty());
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool hasText = viewer_ && GetFileAttributesExW((resultPath(L".txt")).c_str(), GetFileExInfoStandard, &data);
        const uint64_t stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (viewer_ && (force || (hasText && stamp != transcriptStamp_))) {
            transcriptStamp_ = stamp; const string next = SpeechService::read(resultPath(L".txt"));
            if (next != shownText_ || force) { shownText_ = next; DWORD start=0,end=0; SendMessageW(viewer_,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
                const LRESULT first = SendMessageW(viewer_,EM_GETFIRSTVISIBLELINE,0,0);
                SetWindowTextW(viewer_, next.empty() ? uiText("speech.no_text") : transcriptText().c_str());
                if (live_ && start == end) { SendMessageW(viewer_,EM_SETSEL,-1,-1); SendMessageW(viewer_,EM_SCROLLCARET,0,0); }
                else { SendMessageW(viewer_,EM_SETSEL,start,end); SendMessageW(viewer_,EM_LINESCROLL,0,first); } }
        }
        if (!force && revision_ == state.revision) { if (state.busy) InvalidateRect(window_, NULL, FALSE); return; }
        revision_ = state.revision;
        if (install_) {
            const bool failed = !state.failureCode.empty();
            ShowWindow(GetDlgItem(window_,9),failed ? SW_SHOW : SW_HIDE); ShowWindow(GetDlgItem(window_,11),failed ? SW_SHOW : SW_HIDE);
            ShowWindow(detail_,failed && details_ ? SW_SHOW : SW_HIDE);
            SetWindowTextW(GetDlgItem(window_,7),uiText(failed && details_ ? "speech.failure_details" : "speech.steps"));
            SetWindowTextW(GetDlgItem(window_,9),uiText(details_ ? "speech.back_steps" : "speech.failure_details"));
            const wstring report = failed ? failureText(state) : L"";
            if (report != shownDetail_) { shownDetail_ = report; SetWindowTextW(detail_,report.c_str()); }
            EnableWindow(install_, state.enabled && !state.busy); EnableWindow(cancel_, state.busy || state.waiting > 0);
            EnableWindow(language_, state.enabled && !state.busy); EnableWindow(source_, state.enabled && !state.busy); EnableWindow(GetDlgItem(window_, 8), state.enabled && !state.busy);
            SetWindowTextW(install_, uiText(failed ? "speech.retry_install" : state.installed ? "speech.repair" : "speech.install"));
        }
        InvalidateRect(window_, NULL, FALSE);
    }
    /// Render the embedded page or an offline capture, without painting a second application shell.
    void paint(HDC external = NULL) {
        PAINTSTRUCT ps{}; HDC dc = external ? external : BeginPaint(window_, &ps); RECT r{}; GetClientRect(window_, &r);
        HBRUSH bg = CreateSolidBrush(RGB(246,248,252)); FillRect(dc, &r, bg); DeleteObject(bg);
        const int w = width(), h = height(); const auto state = g_speech.state();
        if (!viewer_) {
            surface(dc, 16, 8, w - 32, 204); surface(dc, 16, 228, w - 32, 170);
            const string keys[] = {"check", "runtime", "model", "vad", "verify", "load"};
            int active = -1; for (int i = 0; i < 6; ++i) if (state.preparationStep == keys[i]) active = i;
            const bool failed = !state.failureCode.empty();
            const bool ready = state.installed && !failed && (!state.busy || state.step == "transcribing");
            for (int i = 0; expanded_ && !(details_ && failed) && i < 6; ++i) {
                if (g_speech.model() != "sense" && i == 3) continue;
                const int row = i - (g_speech.model() != "sense" && i > 3 ? 1 : 0);
                const COLORREF dot = ready || active > i ? RGB(24,151,105) : active == i ? state.busy ? RGB(43,105,230) : RGB(192,99,65) : RGB(222,229,240);
                HBRUSH b = CreateSolidBrush(dot); RECT mark{px(34),px(279 + row*19),px(44),px(289 + row*19)}; FillRect(dc,&mark,b); DeleteObject(b);
                text(dc, uiText((g_speech.model() != "sense" && i == 5 ? "speech.load_model" : "speech." + keys[i]).c_str()), 58, 274 + row * 19, w - 190, 23, RGB(68,88,114), small_);
                if (active == i) {
                    const wstring metric = failed ? uiText("speech.step_failed") : state.busy && state.totalBytes.empty() ? std::to_wstring((GetTickCount64() - state.stepStarted) / 1000) + L" " + uiText("speech.seconds") : std::to_wstring(state.percent) + L"%";
                    text(dc, metric, w - 122, 274 + row * 19, 86, 23, RGB(43,105,230), small_, DT_RIGHT | DT_SINGLELINE);
                }
            }
            if (!expanded_ && !(details_ && failed)) text(dc, uiText(("speech." + state.step).c_str()), 34, 282, w - 68, 70, RGB(68,88,114), font_);
            if (failed) {
                HBRUSH fill = CreateSolidBrush(RGB(255,242,239)); RECT error{px(16),px(436),px(w-16),px(524)}; FillRect(dc,&error,fill); DeleteObject(fill);
                text(dc,wstring(uiText("speech.failure_title")) + L" · " + uiText(("speech." + (state.failedStep.empty() ? "unknown_step" : state.failedStep)).c_str()),32,442,w-64,24,RGB(171,58,40),font_);
                text(dc,wstring(uiText(("speech." + state.failureCode).c_str())) + (state.httpStatus.empty() ? L"" : L" (HTTP " + utf8ToWide(state.httpStatus) + L")"),32,474,w-320,44,RGB(145,61,46),small_);
            } else {
            const bool transfer = state.busy && !state.totalBytes.empty() && state.totalBytes != "0";
            text(dc, wstring(uiText(("speech." + state.step).c_str())) + (state.step == "transcribing" ? L"  " + std::to_wstring(state.percent) + L"%" : L"") + (state.waiting ? L"  ·  " + std::to_wstring(state.waiting) + L" " + uiText("speech.waiting") : L""), 20, 407, w - (transfer ? 230 : 40), 28, RGB(68,88,114), font_);
            if (transfer) {
                wchar_t bytes[128]{}; swprintf_s(bytes,L"%.1f / %.1f MB",atof(state.completedBytes.c_str())/1e6,atof(state.totalBytes.c_str())/1e6);
                text(dc,bytes,w-224,407,204,28,RGB(100,116,139),small_,DT_RIGHT | DT_SINGLELINE);
            }
            surface(dc,16,440,w-32,std::max(80,h-496));
            const string model = g_speech.model();
            text(dc,uiText(model == "qwen17" ? "speech.qwen17_download" : model == "qwen" ? "speech.qwen06_download" : "speech.sense_download"),28,448,w-56,20,RGB(31,51,77),small_);
            text(dc,uiText(model == "qwen17" ? "speech.qwen17_config" : model == "qwen" ? "speech.qwen06_config" : "speech.sense_config"),28,470,w-56,30,RGB(68,88,114),small_);
            text(dc,wstring(uiText("speech.hardware_local")) + L" " + std::to_wstring(ramGiB_) + L" GB RAM · " + std::to_wstring(cpuThreads_) + L" " + uiText("speech.hardware_threads") + L" · " + uiText("speech.hardware_cpu"),28,502,w-56,30,RGB(100,116,139),small_);
            }
        } else {
            text(dc, live_ ? uiText("speech.live_title") : std::filesystem::path(wav_).filename().wstring(), 16, 0, w - 324, 26, RGB(31,51,77), font_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            const bool complete = jsonField(SpeechService::read(resultPath(L".json")), "status") == "complete";
            const wstring status = !state.failureCode.empty() ? wstring(uiText("speech.failure_title")) + L" · " + uiText(("speech." + state.failureCode).c_str()) + L" · " + uiText("speech.settings") : uiText(live_ ? state.recording ? state.busy && state.step == "live" ? "speech.live_recording" : "speech.live_loading" : wav_.empty() ? "speech.live_waiting" : "speech.live_ended" : complete ? "speech.result_note" : "speech.partial");
            text(dc, status, 16, 28, w - 32, 32, state.failureCode.empty() ? RGB(100,116,139) : RGB(171,58,40), small_);
            surface(dc, 16, 56, w - 32, std::max(80, h - 116));
        }
        if (!external) EndPaint(window_, &ps);
    }
    /// Render the real nested controls for offline captures, including classic hidden combo/edit text.
    void print(HDC dc) {
        paint(dc);
        for (HWND child = GetWindow(window_,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
            if (!(GetWindowLongW(child,GWL_STYLE) & WS_VISIBLE)) continue;
            RECT r{}; GetWindowRect(child,&r); POINT point{r.left,r.top}; ScreenToClient(window_,&point);
            const int saved = SaveDC(dc); OffsetViewportOrgEx(dc,point.x,point.y,NULL);
            IntersectClipRect(dc,0,0,r.right-r.left,r.bottom-r.top);
            const int native = SaveDC(dc);
            SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
            RestoreDC(dc,native);
            wchar_t kind[32]{}; GetClassNameW(child,kind,32); wstring value;
            const bool combo = wcscmp(kind,L"ComboBox") == 0, edit = wcscmp(kind,L"Edit") == 0;
            if (combo) {
                const LRESULT selected = SendMessageW(child,CB_GETCURSEL,0,0), count = selected >= 0 ? SendMessageW(child,CB_GETLBTEXTLEN,selected,0) : -1;
                if (count >= 0 && count < 2048) { value.resize(static_cast<size_t>(count)+1); SendMessageW(child,CB_GETLBTEXT,selected,reinterpret_cast<LPARAM>(&value[0])); value.resize(static_cast<size_t>(count)); }
            } else if (edit) {
                const int count = std::min(8192,GetWindowTextLengthW(child)); value.resize(static_cast<size_t>(count)+1); GetWindowTextW(child,&value[0],count+1); value.resize(static_cast<size_t>(count));
            }
            if (combo || edit) {
                if (edit) { RECT clear{0,0,r.right-r.left-px(18),r.bottom-r.top}; FillRect(dc,&clear,white_); }
                RECT input{px(7),px(3),r.right-r.left-px(combo ? 24 : 18),r.bottom-r.top-px(3)}; FillRect(dc,&input,white_);
                HGDIOBJ font = SelectObject(dc,font_); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,IsWindowEnabled(child) ? RGB(31,51,77) : RGB(143,155,171));
                DrawTextW(dc,value.c_str(),-1,&input,edit ? DT_LEFT|DT_WORDBREAK|DT_NOPREFIX : DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX); SelectObject(dc,font);
            }
            RestoreDC(dc,saved);
        }
    }
    /// Route local controls and rendering while all navigation remains with the main desktop.
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<SpeechPage*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_CREATE: self->create(); return 0;
        case WM_SIZE: self->layout(); return 0;
        case WM_PAINT: self->paint(); return 0;
        case WM_PRINTCLIENT: self->paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_PRINT: self->print(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: self->refresh(); return 0;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            const bool disclosure = item->CtlID == 7, disabled = (item->itemState & ODS_DISABLED) != 0, primary = item->CtlID == 4 || item->CtlID == 31;
            const COLORREF color = disclosure || !primary ? RGB(255,255,255) : disabled ? RGB(222,229,240) : item->itemState & ODS_SELECTED ? RGB(32,82,186) : RGB(43,105,230);
            HBRUSH brush = CreateSolidBrush(color); FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            if (!disclosure) { HBRUSH edge = CreateSolidBrush(RGB(209,219,234)); FrameRect(item->hDC,&item->rcItem,edge); DeleteObject(edge); }
            wchar_t caption[256]{}; GetWindowTextW(item->hwndItem, caption, 256); wstring label = caption; if (disclosure) label += self->expanded_ ? L"  −" : L"  +";
            RECT r = item->rcItem; SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, disabled ? RGB(143,155,171) : primary ? RGB(255,255,255) : RGB(68,88,114));
            HGDIOBJ font = SelectObject(item->hDC, self->font_); DrawTextW(item->hDC, label.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | (disclosure ? DT_LEFT : DT_CENTER) | DT_NOPREFIX); SelectObject(item->hDC, font);
            if (item->itemState & ODS_FOCUS) { InflateRect(&r,-3,-3); DrawFocusRect(item->hDC,&r); } return TRUE;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: SetBkColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255)); SetTextColor(reinterpret_cast<HDC>(wparam),RGB(31,51,77)); return reinterpret_cast<LRESULT>(self->white_);
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (id == 1 || ((id == 2 || id == 3) && HIWORD(wparam) == CBN_SELCHANGE)) self->configure();
            if (id == 8 && HIWORD(wparam) == CBN_SELCHANGE) { g_speech.selectModel(self->models_[std::max(0,std::min(2,static_cast<int>(SendMessageW(GetDlgItem(window,8),CB_GETCURSEL,0,0))))]); self->refresh(true); }
            if (id == 34 && HIWORD(wparam) == CBN_SELCHANGE) { self->resultModel_ = self->models_[std::max(0,std::min(2,static_cast<int>(SendMessageW(GetDlgItem(window,34),CB_GETCURSEL,0,0))))]; self->transcriptStamp_ = 0; self->refresh(true); }
            if (id == 4) g_speech.prepare(); if (id == 5) g_speech.cancel();
            if (id == 7 || id == 9) {
                if (id == 9) self->details_ = !self->details_;
                else if (self->details_) self->details_ = false;
                else self->expanded_ = !self->expanded_;
                self->refresh(true); InvalidateRect(GetDlgItem(window,7),NULL,TRUE);
            }
            if (id == 31) self->copyText(self->transcriptText());
            if (id == 11) self->copyText(self->failureText(g_speech.state()));
            if (id == 32) {
                const string selected = self->resultModel_;
                if (!g_speech.state().enabled || !g_speech.readyFor(selected)) { g_speech.selectModel(selected); PostMessageW(GetParent(window),WM_COMMAND,436,0); }
                else g_speech.enqueue(self->wav_,false,selected);
            }
            if (id == 33) PostMessageW(GetParent(window),WM_COMMAND,436,0);
            return 0;
        }
        case WM_NCDESTROY:
            KillTimer(window,1); instance() = NULL; SetWindowLongPtrW(window,GWLP_USERDATA,0);
            DeleteObject(self->font_); DeleteObject(self->small_); DeleteObject(self->white_); delete self; return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    /// ASR hints are stable identities, independent of the current UI locale.
    const char* languages_[6] = {"auto","zh","en","yue","ja","ko"};
    /// Native child and controls belong to the desktop's UI thread.
    HWND window_ = NULL, enabled_ = NULL, language_ = NULL, source_ = NULL, install_ = NULL, cancel_ = NULL, viewer_ = NULL, detail_ = NULL;
    /// Page-owned GDI resources and current shell DPI.
    HFONT font_ = NULL, small_ = NULL;
    HBRUSH white_ = NULL;
    int dpi_ = 96, locale_ = -1;
    /// Display installed physical RAM and logical processors; recommendations never claim measured speed.
    unsigned ramGiB_ = 0, cpuThreads_ = 0;
    /// Disclosure state changes presentation only.
    bool expanded_ = true, details_ = false, live_ = false;
    /// Independent history version selection never changes the live engine.
    string resultModel_ = "sense";
    /// Persistent model identities match provider and history combo order.
    const char* models_[3] = {"sense","qwen","qwen17"};
    /// Invalidation markers prevent repeated reads of long text.
    uint64_t revision_ = static_cast<uint64_t>(-1), transcriptStamp_ = 0;
    /// Optional selected recording and currently displayed UTF-8 text.
    wstring wav_;
    string shownText_;
    /// Last sanitized report avoids resetting detail scroll on unrelated UI refreshes.
    wstring shownDetail_;
};
#endif
