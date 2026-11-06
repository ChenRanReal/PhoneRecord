#pragma once
#ifdef _WIN64

/// Two task-focused speech modules embedded in the ordinary desktop shell.
class SpeechPage {
public:
    /// A live sentinel opens SenseVoice; an ordinary path opens historical Qwen transcription.
    static HWND show(HWND parent,const wstring& wav,int dpi) {
        const bool live = wav == L"@live";
        if(instance()) {
            auto* page=reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(instance(),GWLP_USERDATA));
            if(page && page->live_ == live && (live || wav.empty() || page->wav_ == wav)) { page->refresh(true); return instance(); }
            DestroyWindow(instance());
        }
        auto* self=new SpeechPage; self->live_=live; self->wav_=live ? g_speech.state().liveWav : wav; self->dpi_=dpi;
        WNDCLASSEXW type{}; type.cbSize=sizeof(type); type.hInstance=GetModuleHandleW(NULL); type.lpfnWndProc=procedure;
        type.lpszClassName=L"PhoneRecordSpeechPage"; type.hCursor=LoadCursorW(NULL,IDC_ARROW); RegisterClassExW(&type);
        HWND result=CreateWindowExW(WS_EX_CONTROLPARENT,type.lpszClassName,L"",WS_CHILD|WS_CLIPCHILDREN,0,0,1,1,parent,reinterpret_cast<HMENU>(960),type.hInstance,self);
        if(!result) {delete self;return NULL;} instance()=result; return result;
    }
    /// One child follows the currently selected module; no popup owns settings or text.
    static HWND& instance() {static HWND value=NULL;return value;}
    /// Fit both cards to the desktop's minimum size and current DPI.
    static void place(int x,int y,int w,int h,int dpi) {
        if(!instance())return;
        auto* self=reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(instance(),GWLP_USERDATA)); if(!self)return;
        if(self->dpi_!=dpi){self->dpi_=dpi;self->fonts();}
        MoveWindow(instance(),self->px(x),self->px(y),self->px(w),self->px(h),TRUE);self->layout();
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
    /// Settings always occupy the left card; the selected task's text occupies the right card.
    void layout() {
        const int w=width(),h=height(),left=248,right=w-left-32;
        move(1,28,22,204,44); move(44,28,22,204,44);
        move(40,28,82,204,22); move(8,28,108,204,180); move(41,28,108,204,28);
        move(42,28,150,204,22);move(2,28,176,204,200);
        move(43,28,218,204,22);move(3,28,244,204,150);
        move(12,28,286,204,28);move(4,28,332,204,36);move(5,28,378,204,30);
        move(9,28,h-82,204,28);
        move(35,left+20,44,right-124,180);move(36,w-108,44,80,28);
        move(34,left+20,86,right-32,180);
        move(30,left+20,live_?68:128,right-32,std::max(80,h-(live_?140:200)));
        move(10,left+20,68,right-32,std::max(80,h-140));
        move(31,left+20,h-52,148,36);move(32,w-188,h-52,160,36);move(11,w-188,h-52,160,36);
        InvalidateRect(window_,NULL,FALSE);
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
    /// Resolve provider from this module, independent of the version currently being viewed.
    string provider() const {return live_ ? "sense" : SendMessageW(GetDlgItem(window_,8),CB_GETCURSEL,0,0)==1 ? "qwen17" : "qwen";}
    /// Each saved version keeps the established model-specific sidecar paths.
    wstring resultPath(const wchar_t* ext) const {return wav_+SpeechService::resultSuffix(resultModel_)+ext;}
    /// Fill history from the existing recording index; no file picker or second navigation is required.
    void recordings() {
        files_.clear(); HWND choices=GetDlgItem(window_,35);SendMessageW(choices,CB_RESETCONTENT,0,0);
        int selected=0;
        for(const auto& item:scanRecordings()) {
            if(files_.size()>=1000)break;
            wchar_t path[32768]{};GetFullPathNameW(utf8ToWide(g_cfg.recordDir+"\\"+item.file).c_str(),32768,path,NULL);
            if(wav_==path)selected=static_cast<int>(files_.size());files_.push_back(path);
            const wstring label=utf8ToWide(item.date+" "+item.clock+" · "+(item.title.empty()?item.remote:item.title));
            SendMessageW(choices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        if(files_.empty())wav_.clear();else{SendMessageW(choices,CB_SETCURSEL,selected,0);wav_=files_[selected];}
        transcriptStamp_=0;
    }
    /// Create only the controls belonging to the selected task.
    void create() {
        fonts();white_=CreateSolidBrush(RGB(255,255,255));
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory))ramGiB_=static_cast<unsigned>((memory.ullTotalPhys+(1ULL<<29))>>30);
        SYSTEM_INFO system{};GetNativeSystemInfo(&system);cpuThreads_=system.dwNumberOfProcessors;
        if(live_) {
            enabled_=control(L"BUTTON",uiText("speech.live_enable"),WS_TABSTOP|BS_AUTOCHECKBOX|BS_MULTILINE,1);
            SendMessageW(enabled_,BM_SETCHECK,g_speech.state().liveEnabled?BST_CHECKED:BST_UNCHECKED,0);
            control(L"STATIC",L"SenseVoiceSmall · INT8",0,41);
        } else {
            control(L"STATIC",uiText("speech.history_manual"),0,44);
            HWND model=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,8);
            for(const wchar_t* label:{L"Qwen3-ASR 0.6B",L"Qwen3-ASR 1.7B"})SendMessageW(model,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            SendMessageW(model,CB_SETCURSEL,g_speech.model()=="qwen17"?1:0,0);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,35);
            control(L"BUTTON",uiText("button.refresh"),WS_TABSTOP|BS_OWNERDRAW,36);
            HWND versions=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,34);
            for(const char* key:{"speech.saved_sense","speech.saved_qwen","speech.saved_qwen17"})SendMessageW(versions,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText(key)));
            resultModel_=provider();SendMessageW(versions,CB_SETCURSEL,resultModel_=="qwen17"?2:1,0);recordings();
            control(L"BUTTON",uiText("speech.history_start"),WS_TABSTOP|BS_OWNERDRAW,32);
        }
        control(L"STATIC",uiText(live_?"speech.module_model":"speech.history_model"),0,40);
        control(L"STATIC",uiText("speech.language"),0,42);control(L"STATIC",uiText("speech.source"),0,43);
        language_=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,2);
        source_=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,3);
        for(const wchar_t* label:{L"Hugging Face",L"HF-Mirror"})SendMessageW(source_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(source_,CB_SETCURSEL,(live_?g_speech.liveSource():g_speech.source())=="hf-mirror.com"?1:0,0);
        for(int i=0;i<6;++i){SendMessageW(language_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText((string("speech.lang.")+languages_[i]).c_str())));if((live_?g_speech.liveLanguage():g_speech.language())==languages_[i])SendMessageW(language_,CB_SETCURSEL,i,0);}
        control(L"BUTTON",uiText("speech.hardware_button"),WS_TABSTOP|BS_OWNERDRAW,12);
        install_=control(L"BUTTON",uiText("speech.module_install"),WS_TABSTOP|BS_OWNERDRAW,4);
        cancel_=control(L"BUTTON",uiText("speech.module_stop"),WS_TABSTOP|BS_OWNERDRAW,5);
        control(L"BUTTON",uiText("speech.failure_details"),WS_TABSTOP|BS_OWNERDRAW,9);
        viewer_=control(L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,30);
        detail_=control(L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,10);
        SendMessageW(viewer_,EM_SETLIMITTEXT,8*1024*1024,0);SendMessageW(detail_,EM_SETLIMITTEXT,16384,0);
        control(L"BUTTON",uiText("speech.copy"),WS_TABSTOP|BS_OWNERDRAW,31);control(L"BUTTON",uiText("speech.copy_diagnostics"),WS_TABSTOP|BS_OWNERDRAW,11);
        SetTimer(window_,1,200,NULL);refresh(true);layout();
    }
    /// Persist only the settings for this module; setup remains a separate explicit action.
    bool configure() {
        const int selected=static_cast<int>(SendMessageW(language_,CB_GETCURSEL,0,0));if(selected<0||selected>=6)return false;
        const string source=SendMessageW(source_,CB_GETCURSEL,0,0)==1?"hf-mirror.com":"huggingface.co";
        const bool ok=live_?g_speech.configureLive(SendMessageW(enabled_,BM_GETCHECK,0,0)==BST_CHECKED,languages_[selected],source):g_speech.configure(true,languages_[selected],source,true);
        if(!live_&&ok)g_speech.selectModel(provider());
        if(!ok)localError_=uiText("speech.save_failed");refresh(true);return ok;
    }
    /// Show practical configuration guidance on demand, without crowding either task view.
    wstring hardware() const {
        const string model=provider();
        return wstring(uiText(model=="sense"?"speech.sense_download":model=="qwen17"?"speech.qwen17_download":"speech.qwen06_download"))+L"\r\n\r\n"+uiText(model=="sense"?"speech.sense_config":model=="qwen17"?"speech.qwen17_config":"speech.qwen06_config")+L"\r\n\r\n"+uiText("speech.hardware_local")+L" "+std::to_wstring(ramGiB_)+L" GB RAM · "+std::to_wstring(cpuThreads_)+L" "+uiText("speech.hardware_threads")+L"\r\n"+uiText("speech.hardware_cpu");
    }
    /// Poll only changed transcript bytes and keep diagnostics accessible inside the same module.
    void refresh(bool force=false) {
        if(locale_!=g_uiLanguage.load()) {
            locale_=g_uiLanguage.load();force=true;
            const std::map<int,const char*> labels={{1,"speech.live_enable"},{44,"speech.history_manual"},{40,"speech.module_model"},{42,"speech.language"},{43,"speech.source"},{4,"speech.module_install"},{5,"speech.module_stop"},{9,"speech.failure_details"},{12,"speech.hardware_button"},{31,"speech.copy"},{32,"speech.history_start"},{36,"button.refresh"},{11,"speech.copy_diagnostics"}};
            for(const auto& label:labels)if(HWND control=GetDlgItem(window_,label.first))SetWindowTextW(control,uiText(label.second));
            SetWindowTextW(GetDlgItem(window_,40),uiText(live_?"speech.module_model":"speech.history_model"));
            if(!live_){HWND versions=GetDlgItem(window_,34);const LRESULT version=SendMessageW(versions,CB_GETCURSEL,0,0);SendMessageW(versions,CB_RESETCONTENT,0,0);for(const char* key:{"speech.saved_sense","speech.saved_qwen","speech.saved_qwen17"})SendMessageW(versions,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText(key)));SendMessageW(versions,CB_SETCURSEL,version,0);}
            const LRESULT selected=SendMessageW(language_,CB_GETCURSEL,0,0);SendMessageW(language_,CB_RESETCONTENT,0,0);
            for(const char* lang:languages_)SendMessageW(language_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText((string("speech.lang.")+lang).c_str())));SendMessageW(language_,CB_SETCURSEL,selected,0);
        }
        const auto state=g_speech.state();
        if(live_&&wav_!=state.liveWav){wav_=state.liveWav;transcriptStamp_=0;force=true;}
        const bool failure=!state.failureCode.empty()&&(state.operationModel.empty()||state.operationModel==provider());
        if(!failure&&details_&&!lastFailure_.empty()){details_=false;lastFailure_.clear();}
        if(failure&&failureRevision_!=state.revision){if(lastFailure_!=state.failureCode+state.detail){details_=true;advice_=false;lastFailure_=state.failureCode+state.detail;}failureRevision_=state.revision;}
        if(force||revision_!=state.revision) {
            revision_=state.revision;
            EnableWindow(install_,!state.busy);EnableWindow(cancel_,g_speech.pendingFor(provider()));
            for(int id:{2,3,8})EnableWindow(GetDlgItem(window_,id),!state.busy);
            EnableWindow(GetDlgItem(window_,32),!wav_.empty()&&!state.recording&&!state.busy);
            ShowWindow(GetDlgItem(window_,9),failure?SW_SHOW:SW_HIDE);
            SetWindowTextW(install_,uiText(g_speech.readyFor(provider())?"speech.module_repair":"speech.module_install"));
            SetWindowTextW(GetDlgItem(window_,9),uiText(details_?"speech.back_text":"speech.failure_details"));
            SetWindowTextW(GetDlgItem(window_,12),uiText(advice_?"speech.back_text":"speech.hardware_button"));
        }
        const bool panel=details_||advice_;
        ShowWindow(viewer_,panel?SW_HIDE:SW_SHOW);ShowWindow(detail_,panel?SW_SHOW:SW_HIDE);
        ShowWindow(GetDlgItem(window_,31),panel?SW_HIDE:SW_SHOW);ShowWindow(GetDlgItem(window_,11),details_?SW_SHOW:SW_HIDE);
        if(!live_)for(int id:{32,34,35,36})ShowWindow(GetDlgItem(window_,id),panel?SW_HIDE:SW_SHOW);
        if(panel){const wstring report=advice_?hardware():failureText(state);if(report!=shownDetail_){shownDetail_=report;SetWindowTextW(detail_,report.c_str());}}
        WIN32_FILE_ATTRIBUTE_DATA data{};const bool exists=!wav_.empty()&&GetFileAttributesExW(resultPath(L".txt").c_str(),GetFileExInfoStandard,&data);
        const uint64_t stamp=(static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime;
        if(force||stamp!=transcriptStamp_){transcriptStamp_=stamp;const string next=exists?SpeechService::read(resultPath(L".txt")):"";if(force||next!=shownText_){shownText_=next;const LRESULT first=SendMessageW(viewer_,EM_GETFIRSTVISIBLELINE,0,0);SetWindowTextW(viewer_,next.empty()?uiText(live_?"speech.live_empty":"speech.history_empty"):transcriptText().c_str());if(live_){SendMessageW(viewer_,EM_SETSEL,-1,-1);SendMessageW(viewer_,EM_SCROLLCARET,0,0);}else SendMessageW(viewer_,EM_LINESCROLL,0,first);}}
        InvalidateRect(window_,NULL,FALSE);
    }
    /// Paint a compact readiness summary; lengthy setup facts stay in the inline diagnostics panel.
    void paint(HDC external=NULL) {
        PAINTSTRUCT ps{};HDC dc=external?external:BeginPaint(window_,&ps);RECT area{};GetClientRect(window_,&area);
        HBRUSH bg=CreateSolidBrush(RGB(246,248,252));FillRect(dc,&area,bg);DeleteObject(bg);
        const int w=width(),h=height();const auto state=g_speech.state();const bool ready=g_speech.readyFor(provider());
        surface(dc,12,8,236,h-24);surface(dc,260,8,w-272,h-24);
        const bool owns=state.busy&&state.operationModel==provider();
        const bool failed=!state.failureCode.empty()&&(state.operationModel.empty()||state.operationModel==provider());
        const wstring status=!localError_.empty()?localError_:failed?uiText(("speech."+state.failureCode).c_str()):owns?uiText(("speech."+state.step).c_str()):uiText(ready?"speech.module_ready":"speech.module_needed");
        text(dc,status,28,428,204,std::max(44,h-530),failed?RGB(171,58,40):RGB(68,88,114),small_);
        if(owns){RECT track{px(28),px(414),px(232),px(418)};HBRUSH base=CreateSolidBrush(RGB(222,229,240));FillRect(dc,&track,base);DeleteObject(base);track.right=px(28+std::max(3,state.percent)*204/100);HBRUSH bar=CreateSolidBrush(RGB(43,105,230));FillRect(dc,&track,bar);DeleteObject(bar);}
        text(dc,uiText(advice_?"speech.hardware_button":details_?"speech.failure_details":live_?"speech.live_title":"speech.history_choose"),280,20,w-308,26,RGB(31,51,77),font_);
        if(live_&&!details_&&!advice_)text(dc,uiText(!state.liveEnabled?"speech.live_off":state.recording?"speech.live_recording":"speech.live_waiting"),280,46,w-308,22,RGB(100,116,139),small_,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(!external)EndPaint(window_,&ps);
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
    /// Route only local task actions; navigation remains with the shared desktop.
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<SpeechPage*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,wparam,lparam);
        switch(message) {
        case WM_CREATE:self->create();return 0;
        case WM_SIZE:self->layout();return 0;
        case WM_PAINT:self->paint();return 0;
        case WM_PRINT:self->print(reinterpret_cast<HDC>(wparam));return 0;
        case WM_PRINTCLIENT:self->paint(reinterpret_cast<HDC>(wparam));return 0;
        case WM_ERASEBKGND:return 1;
        case WM_TIMER:self->refresh();return 0;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:SetBkColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255));SetTextColor(reinterpret_cast<HDC>(wparam),RGB(31,51,77));return reinterpret_cast<LRESULT>(self->white_);
        case WM_DRAWITEM: {
            const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);const bool primary=item->CtlID==4||item->CtlID==32,disabled=item->itemState&ODS_DISABLED;
            HBRUSH fill=CreateSolidBrush(primary?disabled?RGB(222,229,240):RGB(43,105,230):RGB(255,255,255));FillRect(item->hDC,&item->rcItem,fill);DeleteObject(fill);
            HBRUSH edge=CreateSolidBrush(RGB(209,219,234));FrameRect(item->hDC,&item->rcItem,edge);DeleteObject(edge);
            wchar_t caption[256]{};GetWindowTextW(item->hwndItem,caption,256);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?RGB(143,155,171):primary?RGB(255,255,255):RGB(68,88,114));
            HGDIOBJ font=SelectObject(item->hDC,self->small_);RECT r=item->rcItem;DrawTextW(item->hDC,caption,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(item->hDC,font);if(item->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(item->hDC,&r);}return TRUE;
        }
        case WM_COMMAND: {
            const int id=LOWORD(wparam);const bool changed=HIWORD(wparam)==CBN_SELCHANGE;
            if(id==1||((id==2||id==3||id==8)&&changed))self->configure();
            if(id==4){if(self->configure()){if(!g_speech.state().enabled)g_speech.configure(true,g_speech.language(),g_speech.source(),true);g_speech.prepare(self->provider(),self->live_?g_speech.liveSource():g_speech.source());}}
            if(id==5)g_speech.cancelModel(self->provider());
            if(id==9){self->details_=!self->details_;self->advice_=false;}
            if(id==12){self->advice_=!self->advice_;self->details_=false;}
            if(id==36)self->recordings();
            if(id==35&&changed){const int i=static_cast<int>(SendMessageW(GetDlgItem(window,35),CB_GETCURSEL,0,0));if(i>=0&&i<static_cast<int>(self->files_.size()))self->wav_=self->files_[i];self->transcriptStamp_=0;}
            if(id==34&&changed){const int i=static_cast<int>(SendMessageW(GetDlgItem(window,34),CB_GETCURSEL,0,0));self->resultModel_=i==2?"qwen17":i==1?"qwen":"sense";self->transcriptStamp_=0;}
            if(id==31)self->copyText(self->transcriptText());if(id==11)self->copyText(self->failureText(g_speech.state()));
            if(id==32&&self->configure()){
                if(!g_speech.readyFor(self->provider()))self->localError_=uiText("speech.module_needed");
                else {self->resultModel_=self->provider();SendMessageW(GetDlgItem(window,34),CB_SETCURSEL,self->resultModel_=="qwen17"?2:1,0);if(!g_speech.enqueue(self->wav_,false,self->resultModel_))self->localError_=uiText("speech.admission_failed");else self->localError_.clear();}
            }
            self->refresh(true);return 0;
        }
        case WM_NCDESTROY:KillTimer(window,1);instance()=NULL;SetWindowLongPtrW(window,GWLP_USERDATA,0);DeleteObject(self->font_);DeleteObject(self->small_);DeleteObject(self->white_);delete self;return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    /// Native controls and brushes belong to this one UI child.
    HWND window_=NULL,enabled_=NULL,language_=NULL,source_=NULL,install_=NULL,cancel_=NULL,viewer_=NULL,detail_=NULL;
    HFONT font_=NULL,small_=NULL;HBRUSH white_=NULL;
    /// Task identity, inline auxiliary panel and persistent file-version selection.
    bool live_=false,details_=false,advice_=false;
    int dpi_=96,locale_=-1;unsigned ramGiB_=0,cpuThreads_=0;
    uint64_t revision_=static_cast<uint64_t>(-1),failureRevision_=0,transcriptStamp_=0;
    wstring wav_,shownDetail_,localError_;string shownText_,resultModel_="sense",lastFailure_;
    std::vector<wstring> files_;
    const char* languages_[6]={"auto","zh","en","yue","ja","ko"};
};
#endif
