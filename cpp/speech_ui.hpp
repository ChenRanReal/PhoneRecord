#pragma once
#ifdef _WIN64

#include "virtual_audio.hpp"

/// Two task-focused speech modules embedded in the ordinary desktop shell.
class SpeechPage {
public:
    /// A live sentinel opens SenseVoice; an ordinary path opens historical Qwen transcription.
    /// savedLive opens that recording's already saved live-caption text.
    static HWND show(HWND parent,const wstring& wav,int dpi,bool savedLive=false) {
        const bool live = wav == L"@live";
        if(instance()) {
            auto* page=reinterpret_cast<SpeechPage*>(GetWindowLongPtrW(instance(),GWLP_USERDATA));
            if(page && page->live_ == live && (live || wav.empty() || page->wav_ == wav)) {
                if(!live && savedLive) page->showSavedLive();
                page->refresh(true); return instance();
            }
            DestroyWindow(instance());
        }
        auto* self=new SpeechPage; self->live_=live; self->savedLive_=!live && savedLive; self->wav_=live ? g_speech.state().liveWav : wav; self->dpi_=dpi;
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
        RECT next{self->px(x),self->px(y),self->px(x+w),self->px(y+h)};RECT current{};GetWindowRect(instance(),&current);
        POINT origin{current.left,current.top};ScreenToClient(GetParent(instance()),&origin);
        if(origin.x!=next.left||origin.y!=next.top||(current.right-current.left)!=(next.right-next.left)||(current.bottom-current.top)!=(next.bottom-next.top)){MoveWindow(instance(),next.left,next.top,next.right-next.left,next.bottom-next.top,TRUE);self->layout();}
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
        move(1,28,16,204,36); move(45,28,16,204,36);
        // Both pages share one grid: engine or model, input method, playback device, dependency, cable state.
        move(50,28,54,204,18); move(51,28,72,204,140);
        // The live page labels its SenseVoice block under the engine row; the transcription page labels its model combo.
        if(live_) move(40,28,108,204,18); else move(40,28,54,204,18);
        move(8,28,72,204,160);
        move(52,28,108,204,18); move(53,28,126,204,160);
        move(54,28,160,204,18); move(55,28,178,204,140);
        move(56,28,214,204,76); move(58,28,214,204,76);
        move(41,28,128,204,24);
        move(42,28,158,204,18); move(2,28,176,204,160);
        move(43,28,210,204,18); move(3,28,228,204,120);
        move(60,28,292,204,56); move(59,28,352,204,28);
        if(imeSelected()) move(5,28,384,120,28);
        else { move(12,28,262,204,28); move(4,28,296,204,32); move(5,28,334,204,28); }
        // The focus-hold row only exists on the transcription page with an input method; it squeezes
        // the hint and cable status to make room without moving the install and stop rows. This must
        // run after the shared grid above so its positions win.
        if(!live_&&imeSelected()){move(61,28,198,204,20);move(58,28,218,204,88);move(60,28,308,204,46);move(59,28,354,204,28);}
        move(9,28,h-82,204,28);
        move(35,left+20,44,right-124,180);move(36,w-108,44,80,28);
        move(34,left+20,86,right-56,220);
        move(57,live_?280:w-46,live_?18:90,22,22);
        move(30,left+20,live_?68:148,right-32,std::max(80,h-(live_?140:220)));
        move(10,left+20,68,right-32,std::max(80,h-140));
        move(31,left+20,h-52,148,36);move(32,w-188,h-52,160,36);move(11,w-188,h-52,160,36);
        if(w!=laidWidth_||h!=laidHeight_){laidWidth_=w;laidHeight_=h;InvalidateRect(window_,NULL,FALSE);}
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
    string provider() const {
        if(live_) return "sense";
        const int index=static_cast<int>(SendMessageW(GetDlgItem(window_,8),CB_GETCURSEL,0,0));
        const string id=index>=0&&index<static_cast<int>(historyModels_.size())?historyModels_[index]:"qwen";
        if(id!="ime") return id;
        const LiveImeEntry* ime=selectedIme();
        return ime?ime->model:"";
    }
    /// Left-side transcription choices: the input-method engine first, then the offline models.
    /// The input method itself is picked in the same second row the live page uses.
    void fillHistoryModels() {
        HWND model=GetDlgItem(window_,8); if(!model) return;
        string current;
        const LRESULT selected=SendMessageW(model,CB_GETCURSEL,0,0);
        if(!historyModels_.empty()&&selected>=0&&selected<static_cast<LRESULT>(historyModels_.size())) current=historyModels_[selected];
        if(current.empty()) current=g_speech.model()=="qwen17"?"qwen17":g_speech.model()=="sense"?"sense":"qwen";
        historyModels_.clear(); SendMessageW(model,CB_RESETCONTENT,0,0);
        const wstring offline=uiText("speech.trait_offline");
        auto add=[&](const string& id,const wstring& label){ historyModels_.push_back(id); SendMessageW(model,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())); if(id==current) SendMessageW(model,CB_SETCURSEL,static_cast<WPARAM>(historyModels_.size()-1),0); };
        add("ime",uiText("speech.engine_ime"));
        add("qwen",L"Qwen3-ASR 0.6B · "+offline); add("qwen17",L"Qwen3-ASR 1.7B · "+offline); add("sense",L"SenseVoice · "+offline);
        if(SendMessageW(model,CB_GETCURSEL,0,0)<0) SendMessageW(model,CB_SETCURSEL,1,0);
        SendMessageW(model,CB_SETDROPPEDWIDTH,420,0);
    }
    /// Play one saved recording into the selected input method without blocking the window.
    void startImeFile(const string& model) {
        if(wav_.empty()||imeFileRunning_.exchange(true)) return;
        string savedTip, savedProfile, savedDevice; g_speech.liveImeSelection(savedTip, savedProfile, savedDevice);
        const wstring device=findVirtualCableId(utf8ToWide(savedDevice));
        if(device.empty()){ imeFileRunning_=false; notice(uiText("speech.ime_no_device"),true); refresh(true); return; }
        localError_=uiText("speech.ime_file_running"); resultModel_=model; if(HWND versions=GetDlgItem(window_,34))SendMessageW(versions,CB_SETCURSEL,versionIndex(model),0); transcriptStamp_=0; refresh(true);
        SetForegroundWindow(window_); if(viewer_) SetFocus(viewer_);
        imeActivateModel(model, fileProfileHold()); Sleep(200); if(viewer_) SetFocus(viewer_);
        // The caption worker opens WeChat's bar itself, because an old bar has to be closed first.
        if(model!="wetype") openImeVoice(model, false);
        const HWND owner=window_; const wstring wav=wav_;
        std::thread([owner,wav,model,device]{ string text; const int code=imePlayExistingWav(wav,L"CABLE Input",text,model,device); if(IsWindow(owner)) PostMessageW(owner,WM_APP+3,code,0); }).detach();
    }
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
        if(!live_&&(resultModel_=="sense"||imeResultModel(resultModel_))){const string model=sharedCaptionModel();resultModel_=imeResultModel(model)?model:"sense";if(HWND versions=GetDlgItem(window_,34))SendMessageW(versions,CB_SETCURSEL,versionIndex(resultModel_),0);}
        transcriptStamp_=0;
    }
    /// Saved-result menu order. WeChat input stays last so the existing SenseVoice and Qwen positions do not move.
    int versionIndex(const string& model) const {
        const char* models[] = {"sense","qwen","qwen17","wetype","doubao","iflytek","sogou","windows"};
        for (int index = 0; index < 8; ++index) if (model == models[index]) return index;
        return 0;
    }
    string versionModel(int index) const {
        const char* models[] = {"sense","qwen","qwen17","wetype","doubao","iflytek","sogou","windows"};
        return index >= 0 && index < 8 ? models[index] : "sense";
    }
    /// Model written inside the shared live sidecar. Qwen files are separate and do not use this.
    string sharedCaptionModel() const {
        if (wav_.empty()) return "";
        return jsonField(SpeechService::read(wav_ + L".transcript.json"), "model");
    }
    /// Visible name of the engine that produced the text now on screen.
    wstring sourceCaptionFor(const string& model) const {
        if (model=="wetype") return uiText("speech.source_wetype");
        if (model=="doubao") return uiText("speech.source_doubao");
        if (model=="iflytek") return uiText("speech.source_iflytek");
        if (model=="sogou") return uiText("speech.source_sogou");
        if (model=="windows") return uiText("speech.source_windows");
        if (model.find("sense")!=string::npos) return uiText("speech.engine_sense");
        return L"";
    }
    wstring sourceCaption() const { return sourceCaptionFor(captionModel_); }
    void fillVersions(int keep) {
        HWND versions=GetDlgItem(window_,34); if(!versions) return;
        SendMessageW(versions,CB_RESETCONTENT,0,0);
        for(const char* key:{"speech.saved_sense","speech.saved_qwen","speech.saved_qwen17","speech.saved_wetype","speech.saved_doubao","speech.saved_iflytek","speech.saved_sogou","speech.saved_windows"}) SendMessageW(versions,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText(key)));
        SendMessageW(versions,CB_SETCURSEL,keep,0);
        SendMessageW(versions,CB_SETDROPPEDWIDTH,420,0);
    }
    /// Show the supported input method's own icon. The tooltip lists those input methods by their system names.
    void applyImeIcon() {
        if(imeIcon_){DestroyIcon(imeIcon_);imeIcon_=NULL;}
        const string shown=live_?captionModel_:resultModel_;
        wstring names; imeIcon_=supportedImeIcon(names, imeResultModel(shown)?shown:""); supportedNames_=names;
        HWND icon=GetDlgItem(window_,57); if(!icon) return;
        if(imeIcon_) SendMessageW(icon,STM_SETICON,reinterpret_cast<WPARAM>(imeIcon_),0);
        ShowWindow(icon,imeIcon_?SW_SHOW:SW_HIDE);
        tooltipText_=wstring(uiText("speech.ime_icon_tip"));
        if(!names.empty()) tooltipText_+=L"\n"+names;
        if(!tooltip_) tooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASS,NULL,WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,NULL,GetModuleHandleW(NULL),NULL);
        TOOLINFOW info{}; info.cbSize=sizeof(info); info.uFlags=TTF_IDISHWND|TTF_SUBCLASS; info.hwnd=window_; info.uId=reinterpret_cast<UINT_PTR>(icon); info.lpszText=const_cast<wchar_t*>(tooltipText_.c_str());
        if(!SendMessageW(tooltip_,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))) SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        else SendMessageW(tooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));
        SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,320);
    }
    bool imeSelected() const {
        // The live page picks the engine first; the transcription page picks the input method in its model list.
        if(!live_) return imeResultModel(provider());
        HWND engine=GetDlgItem(window_,51); if(!engine) return false;
        return SendMessageW(engine,CB_GETCURSEL,0,0)==0;
    }
    /// The input method currently shown in the combo, or null when the list is empty.
    const LiveImeEntry* selectedIme() const {
        const int index=static_cast<int>(SendMessageW(GetDlgItem(window_,53),CB_GETCURSEL,0,0));
        return index>=0 && index<static_cast<int>(imes_.size()) ? &imes_[index] : nullptr;
    }
    /// WeChat's voice bar draws the recognized sentence itself, so live captions stay in that bar.
    bool liveOwnCaption() const {
        if(!live_||!imeSelected())return false;
        const LiveImeEntry* ime=selectedIme();
        return ime&&ime->model=="wetype";
    }
    /// Both pages show the same rows: engine, input method, playback device and the cable state.
    void showEngine() {
        const bool ime=imeSelected();
        if(live_){
            for(int id:{52,53,54,55,56}) if(HWND child=GetDlgItem(window_,id)) ShowWindow(child,ime?SW_SHOW:SW_HIDE);
            for(int id:{40,41,42,2,43,3,12,4,5}) if(HWND child=GetDlgItem(window_,id)) ShowWindow(child,ime?SW_HIDE:SW_SHOW);
        } else {
            // An input method has no recognition language or model download; the freed rows report the cable instead.
            for(int id:{52,53,54,55,58,61}) if(HWND child=GetDlgItem(window_,id)) ShowWindow(child,ime?SW_SHOW:SW_HIDE);
            for(int id:{42,2,43,3,12,4}) if(HWND child=GetDlgItem(window_,id)) ShowWindow(child,ime?SW_HIDE:SW_SHOW);
            if(HWND sense=GetDlgItem(window_,41)) ShowWindow(sense,(!ime&&provider()=="sense")?SW_SHOW:SW_HIDE);
        }
        if(HWND status=GetDlgItem(window_,60)) ShowWindow(status,ime?SW_SHOW:SW_HIDE);
        if(HWND cable=GetDlgItem(window_,59)) ShowWindow(cable,ime&&!cablePresent_?SW_SHOW:SW_HIDE);
        layout();
    }
    /// Persist the visible input method and playback endpoint without changing Windows defaults.
    void applyImeSelection() {
        const LiveImeEntry* ime=selectedIme();
        const int deviceIndex=static_cast<int>(SendMessageW(GetDlgItem(window_,55),CB_GETCURSEL,0,0));
        const string device=deviceIndex>=0 && deviceIndex<static_cast<int>(devices_.size()) ? wideToUtf8(devices_[deviceIndex].id) : "";
        g_speech.selectLiveIme(ime?ime->tip:"", ime?ime->profile:"", device);
    }
    /// Reload installed text services and render endpoints. Product names stay in the system language.
    void loadImeChoices(bool persist) {
        imes_=enumerateEnabledImes(); devices_=enumeratePlaybackDevices();
        string tip,profile,device; g_speech.liveImeSelection(tip,profile,device);
        HWND imes=GetDlgItem(window_,53); const LRESULT previous=SendMessageW(imes,CB_GETCURSEL,0,0);
        SendMessageW(imes,CB_RESETCONTENT,0,0);
        int pick=-1, firstSupported=-1;
        for(int index=0; index<static_cast<int>(imes_.size()); ++index) {
            const auto& item=imes_[index];
            const wstring shown=item.supported?sourceCaptionFor(item.model):item.name;
            const wstring label=(shown.empty()?item.name:shown)+L" · "+uiText(item.supported?"speech.ime_supported":"speech.ime_unsupported_tag");
            SendMessageW(imes,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(imes_[index].supported && firstSupported<0) firstSupported=index;
            if(imes_[index].tip==tip && imes_[index].profile==profile) pick=index;
        }
        if(pick<0) pick=firstSupported>=0?firstSupported:(imes_.empty()?-1:0);
        if(pick<0 && previous>=0 && previous<static_cast<LRESULT>(imes_.size())) pick=static_cast<int>(previous);
        if(pick>=0) SendMessageW(imes,CB_SETCURSEL,pick,0);
        HWND outputs=GetDlgItem(window_,55); SendMessageW(outputs,CB_RESETCONTENT,0,0);
        int devicePick=-1, cable=-1;
        for(int index=0; index<static_cast<int>(devices_.size()); ++index) {
            SendMessageW(outputs,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices_[index].name.c_str()));
            if(utf8ToWide(device)==devices_[index].id) devicePick=index;
            if(cable<0 && devices_[index].name.rfind(L"CABLE Input",0)==0) cable=index;
        }
        if(devicePick<0) devicePick=cable;
        if(devicePick>=0) SendMessageW(outputs,CB_SETCURSEL,devicePick,0);
        if(persist) applyImeSelection();
        showEngine();
    }
    /// Create only the controls belonging to the selected task.
    void create() {
        fonts();white_=CreateSolidBrush(RGB(255,255,255));
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory))ramGiB_=static_cast<unsigned>((memory.ullTotalPhys+(1ULL<<29))>>30);
        SYSTEM_INFO system{};GetNativeSystemInfo(&system);cpuThreads_=system.dwNumberOfProcessors;
        if(live_) {
            enabled_=control(L"BUTTON",uiText("speech.live_enable"),WS_TABSTOP|BS_AUTOCHECKBOX|BS_MULTILINE,1);
            SendMessageW(enabled_,BM_SETCHECK,g_speech.state().liveEnabled?BST_CHECKED:BST_UNCHECKED,0);
            control(L"STATIC",uiText("speech.live_engine"),0,50);
            HWND engine=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,51);
            SendMessageW(engine,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText("speech.engine_ime")));
            SendMessageW(engine,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText("speech.engine_sense")));
            SendMessageW(engine,CB_SETCURSEL,g_speech.liveEngine()=="ime"?0:1,0);
            control(L"STATIC",uiText("speech.ime_pick"),0,52);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,53);
            control(L"STATIC",uiText("speech.playback_device"),0,54);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,55);
            control(L"STATIC",uiText("speech.ime_depend"),SS_LEFT|SS_EDITCONTROL|SS_NOPREFIX,56);
            control(L"STATIC",L"",SS_LEFT|SS_EDITCONTROL|SS_NOPREFIX,60);
            control(L"BUTTON",uiText("speech.cable_install"),WS_TABSTOP|BS_OWNERDRAW,59);
            control(L"STATIC",L"SenseVoiceSmall · INT8",0,41);
            loadImeChoices(true);
        } else {
            enabled_=control(L"BUTTON",uiText("speech.history_enable"),WS_TABSTOP|BS_AUTOCHECKBOX|BS_MULTILINE,45);
            SendMessageW(enabled_,BM_SETCHECK,g_speech.state().historyEnabled?BST_CHECKED:BST_UNCHECKED,0);
            HWND model=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,8);
            fillHistoryModels();
            // The input method and playback device are the same rows the live page shows.
            control(L"STATIC",uiText("speech.ime_pick"),0,52);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,53);
            control(L"STATIC",uiText("speech.playback_device"),0,54);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,55);
            control(L"STATIC",uiText("speech.model_hint_ime"),SS_LEFT|SS_EDITCONTROL|SS_NOPREFIX,58);
            // The focus hold is a user decision: the input method types into the focused input box
            // of other apps, which cannot be intercepted, so the choice is documented right here.
            control(L"BUTTON",uiText("speech.ime_focus_pause"),WS_TABSTOP|BS_AUTOCHECKBOX,61);
            SendMessageW(GetDlgItem(window_,61),BM_SETCHECK,imeFocusPause().load()?BST_CHECKED:BST_UNCHECKED,0);
            control(L"STATIC",L"SenseVoiceSmall · INT8",0,41);
            loadImeChoices(true);
            control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,35);
            control(L"BUTTON",uiText("button.refresh"),WS_TABSTOP|BS_OWNERDRAW,36);
            control(L"STATIC",L"",SS_LEFT|SS_EDITCONTROL|SS_NOPREFIX,60);
            control(L"BUTTON",uiText("speech.cable_install"),WS_TABSTOP|BS_OWNERDRAW,59);
            HWND versions=control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,34);
            if(savedLive_){const string model=sharedCaptionModel();resultModel_=imeResultModel(model)?model:"sense";} else resultModel_=provider();
            fillVersions(versionIndex(resultModel_));recordings();
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
        control(L"STATIC",L"",SS_ICON|SS_NOTIFY|SS_CENTERIMAGE,57);
        applyImeIcon();
        if(HWND hint=GetDlgItem(window_,58)) SendMessageW(hint,WM_SETFONT,reinterpret_cast<WPARAM>(small_),TRUE);
        if(HWND depend=GetDlgItem(window_,56)) SendMessageW(depend,WM_SETFONT,reinterpret_cast<WPARAM>(small_),TRUE);
        showEngine();
        SetTimer(window_,1,200,NULL);refresh(true);layout();
    }
    /// Show the SenseVoice sidecar saved beside this recording.
    void showSavedLive() {
        savedLive_=true; const string model=sharedCaptionModel(); resultModel_=imeResultModel(model)?model:"sense"; transcriptStamp_=0;
        if(HWND versions=GetDlgItem(window_,34)) SendMessageW(versions,CB_SETCURSEL,versionIndex(resultModel_),0);
    }
    wstring clockText(uint64_t ms) const {
        unsigned seconds=static_cast<unsigned>(ms/1000), minutes=seconds/60; seconds%=60; const unsigned hours=minutes/60; minutes%=60;
        wchar_t buffer[32];
        if(hours) swprintf(buffer,32,L"%u:%02u:%02u",hours,minutes,seconds); else swprintf(buffer,32,L"%02u:%02u",minutes,seconds);
        return buffer;
    }
    wstring playProgressText() const { return wstring(uiText("speech.play_progress"))+L"  "+clockText(shownPlaySec_*1000)+L" / "+clockText(shownPlayTotalMs_); }
    /// While an input method is playing a file, keep the transcript and the clock moving.
    void showImePlayback() {
        if(live_||!ImeFileSession::running().load()){ if(shownPlayTotalMs_){shownPlayTotalMs_=0;InvalidateRect(window_,NULL,FALSE);} return; }
        imeFileRunning_=true;
        string model, original, polished;
        if(ImeFileSession::snapshot(model, original, polished)){
            if(!model.empty()&&resultModel_!=model){ resultModel_=model; if(HWND versions=GetDlgItem(window_,34)) SendMessageW(versions,CB_SETCURSEL,versionIndex(model),0); }
            string next;
            if(!original.empty()) next+=wideToUtf8(uiText("speech.original"))+"\n"+imePanelUtf8(original)+"\n";
            if(!polished.empty()) next+=wideToUtf8(uiText("speech.polished"))+"\n"+imePanelUtf8(polished);
            if(imePanelUtf8(original).empty()&&imePanelUtf8(polished).empty()) next.clear();
            if(next!=shownText_){
                shownText_=std::move(next);
                SetWindowTextW(viewer_,transcriptText().c_str());
                const int length=GetWindowTextLengthW(viewer_);
                SendMessageW(viewer_,EM_SETSEL,length,length); SendMessageW(viewer_,EM_SCROLLCARET,0,0);
            }
        }
        const int opened=ImeFileSession::voiceOpened().load();
        // The recording keeps playing when the user works in another window; only the hint changes.
        const bool away=!imeForegroundOwned();
        if(away!=away_){away_=away;if(away)notice(uiText("speech.ime_background"),false);else if(localError_==uiText("speech.ime_background")){localError_.clear();localErrorFail_=false;}}
        if(opened==3) notice(uiText("speech.ime_voice_mic"),true);
        else if(opened==2) notice(uiText("speech.ime_voice_closed"),true);
        else if(opened==1&&!away) notice(uiText("speech.ime_voice_opened"),false);
        const uint64_t total=ImeFileSession::totalMs().load(), second=ImeFileSession::playedMs().load()/1000;
        if(second!=shownPlaySec_||total!=shownPlayTotalMs_){ shownPlaySec_=second; shownPlayTotalMs_=total; InvalidateRect(window_,NULL,FALSE); }
    }
    /// Message shown while the virtual cable is missing or being installed.
    const wchar_t* cableStatusText() const {
        const auto phase=cableInstall().phase;
        if(phase==CableInstallState::downloading||phase==CableInstallState::extracting||phase==CableInstallState::installing||phase==CableInstallState::verifying) return uiText("speech.cable_working");
        if(phase==CableInstallState::failed){
            const string& failure=cableInstall().failure;
            if(failure=="denied") return uiText("speech.cable_denied");
            if(failure=="download") return uiText("speech.cable_download_failed");
            if(failure=="verify") return uiText("speech.cable_verify_failed");
            return uiText("speech.cable_setup_failed");
        }
        return uiText("speech.cable_missing");
    }
    /// Show one inline message and remember whether it must read as a failure.
    void notice(const wstring& text, bool fail) { localError_ = text; localErrorFail_ = fail; }
    /// Persist only the settings for this module; setup remains a separate explicit action.
    bool configure() {
        const int selected=static_cast<int>(SendMessageW(language_,CB_GETCURSEL,0,0));if(selected<0||selected>=6)return false;
        const string source=SendMessageW(source_,CB_GETCURSEL,0,0)==1?"hf-mirror.com":"huggingface.co";
        const bool on=SendMessageW(enabled_,BM_GETCHECK,0,0)==BST_CHECKED;
        const bool ok=live_?g_speech.configureLive(on,languages_[selected],source):g_speech.configureHistory(on,languages_[selected],source);
        if(!live_&&ok){const string model=provider(); if(model=="sense"||model=="qwen"||model=="qwen17") g_speech.selectModel(model);}
        if(!ok)notice(uiText("speech.save_failed"),true);refresh(true);return ok;
    }
    /// Show practical configuration guidance on demand, without crowding either task view.
    wstring hardware() const {
        const string model=provider();
        if (imeResultModel(model)) return uiText("speech.ime_depend");
        return wstring(uiText(model=="sense"?"speech.sense_download":model=="qwen17"?"speech.qwen17_download":"speech.qwen06_download"))+L"\r\n\r\n"+uiText(model=="sense"?"speech.sense_config":model=="qwen17"?"speech.qwen17_config":"speech.qwen06_config")+L"\r\n\r\n"+uiText("speech.hardware_local")+L" "+std::to_wstring(ramGiB_)+L" GB RAM · "+std::to_wstring(cpuThreads_)+L" "+uiText("speech.hardware_threads")+L"\r\n"+uiText("speech.hardware_cpu");
    }
    /// Poll only changed transcript bytes and keep diagnostics accessible inside the same module.
    void refresh(bool force=false) {
        if(locale_!=g_uiLanguage.load()) {
            locale_=g_uiLanguage.load();force=true;
            const std::map<int,const char*> labels={{1,"speech.live_enable"},{45,"speech.history_enable"},{40,"speech.module_model"},{42,"speech.language"},{43,"speech.source"},{4,"speech.module_install"},{5,"speech.module_stop"},{9,"speech.failure_details"},{12,"speech.hardware_button"},{31,"speech.copy"},{32,"speech.history_start"},{36,"button.refresh"},{11,"speech.copy_diagnostics"},{50,"speech.live_engine"},{52,"speech.ime_pick"},{54,"speech.playback_device"},{56,"speech.ime_depend"},{58,"speech.model_hint_ime"},{61,"speech.ime_focus_pause"}};
            for(const auto& label:labels)if(HWND control=GetDlgItem(window_,label.first))SetWindowTextW(control,uiText(label.second));
            SetWindowTextW(GetDlgItem(window_,40),uiText(live_?"speech.module_model":"speech.history_model"));
            if(!live_){const LRESULT version=SendMessageW(GetDlgItem(window_,34),CB_GETCURSEL,0,0);fillVersions(static_cast<int>(version));}
            const LRESULT selected=SendMessageW(language_,CB_GETCURSEL,0,0);SendMessageW(language_,CB_RESETCONTENT,0,0);
            for(const char* lang:languages_)SendMessageW(language_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText((string("speech.lang.")+lang).c_str())));SendMessageW(language_,CB_SETCURSEL,selected,0);
            if(live_){HWND engine=GetDlgItem(window_,51);const LRESULT which=SendMessageW(engine,CB_GETCURSEL,0,0);SendMessageW(engine,CB_RESETCONTENT,0,0);SendMessageW(engine,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText("speech.engine_ime")));SendMessageW(engine,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(uiText("speech.engine_sense")));SendMessageW(engine,CB_SETCURSEL,which,0);} else fillHistoryModels();
            loadImeChoices(false);
            applyImeIcon();
        }
        if(imeSelected() && ++imePoll_>=25){imePoll_=0;loadImeChoices(false);}
        if(ImeFileSession::running().load()) imeFileRunning_=true;
        const auto state=g_speech.state();
        // Both pages can drive an input method, so both watch the cable that the playback depends on.
        if(imeSelected()){
            const bool was=cablePresent_;
            if(++cablePoll_>=5||cablePhase_<0){cablePoll_=0;cablePresent_=virtualCableInstalled();}
            const auto phase=cableInstall().phase;
            if(cablePhase_!=static_cast<int>(phase)){cablePhase_=static_cast<int>(phase);force=true;
                if(phase==CableInstallState::done&&cablePresent_)notice(uiText("speech.cable_done"),false);}
            if(was!=cablePresent_){force=true;showEngine();}
            if(HWND cable=GetDlgItem(window_,59)){
                const bool working=phase==CableInstallState::downloading||phase==CableInstallState::extracting||phase==CableInstallState::installing||phase==CableInstallState::verifying;
                const wchar_t* label=working?uiText("speech.cable_working"):phase==CableInstallState::failed?uiText("speech.cable_retry"):uiText("speech.cable_install");
                if(cableButton_!=label){cableButton_=label;SetWindowTextW(cable,label);}
                ShowWindow(cable,(imeSelected()&&!cablePresent_)?SW_SHOW:SW_HIDE);
                EnableWindow(cable,!working);
            }
            if(HWND status=GetDlgItem(window_,60)){
                const wstring text=cablePresent_?uiText("speech.cable_ready"):cableStatusText();
                if(cableStatus_!=text){cableStatus_=text;SetWindowTextW(status,text.c_str());InvalidateRect(status,NULL,TRUE);}
            }
        }
        if(live_&&wav_!=state.liveWav){wav_=state.liveWav;transcriptStamp_=0;force=true;}
        const bool failure=!state.failureCode.empty()&&(state.operationModel.empty()||state.operationModel==provider());
        if(!failure&&details_&&!lastFailure_.empty()){details_=false;lastFailure_.clear();}
        if(failure&&failureRevision_!=state.revision){if(lastFailure_!=state.failureCode+state.detail){details_=true;advice_=false;lastFailure_=state.failureCode+state.detail;}failureRevision_=state.revision;}
        if(force||revision_!=state.revision) {
            revision_=state.revision;
            const bool locked=!live_&&(state.busy||imeFileRunning_);
            EnableWindow(install_,(live_||state.historyEnabled)&&!locked&&(live_||!imeResultModel(provider())));
            EnableWindow(cancel_,state.busy||imeFileRunning_||g_speech.pendingFor(provider()));
            for(int id:{2,3,8,34,35,36,45,12,53,55})EnableWindow(GetDlgItem(window_,id),!locked);
            EnableWindow(GetDlgItem(window_,32),!locked&&state.historyEnabled&&!wav_.empty()&&!state.recording&&!state.busy);
            ShowWindow(GetDlgItem(window_,9),failure?SW_SHOW:SW_HIDE);
            SetWindowTextW(install_,uiText(g_speech.readyFor(provider())?"speech.module_repair":"speech.module_install"));
            SetWindowTextW(GetDlgItem(window_,9),uiText(details_?"speech.back_text":"speech.failure_details"));
            SetWindowTextW(GetDlgItem(window_,12),uiText(advice_?"speech.back_text":"speech.hardware_button"));
        }
        const bool panel=details_||advice_;
        const auto reveal=[&](HWND child,bool visible){if(!child)return;const bool shown=(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE)!=0;if(shown!=visible)ShowWindow(child,visible?SW_SHOW:SW_HIDE);};
        reveal(viewer_,!panel);reveal(detail_,panel);reveal(GetDlgItem(window_,31),!panel);reveal(GetDlgItem(window_,11),details_);
        if(!live_)for(int id:{32,34,35,36,57})reveal(GetDlgItem(window_,id),!panel&&(id!=57||imeIcon_));
        if(live_)reveal(GetDlgItem(window_,57),!panel&&imeIcon_);
        if(panel){const wstring report=advice_?hardware():failureText(state);if(report!=shownDetail_){shownDetail_=report;SetWindowTextW(detail_,report.c_str());}}
        WIN32_FILE_ATTRIBUTE_DATA data{};const bool exists=!wav_.empty()&&GetFileAttributesExW(resultPath(L".txt").c_str(),GetFileExInfoStandard,&data);
        const uint64_t stamp=(static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime;
        const string fileModel=sharedCaptionModel();
        const bool separate=resultModel_=="qwen"||resultModel_=="qwen17";
        const bool matches=live_||separate||(resultModel_=="sense" ? fileModel.find("sense")!=string::npos||fileModel.empty() : fileModel==resultModel_);
        if(force||stamp!=transcriptStamp_||fileModel!=captionModel_){
            const string previousCaption = captionModel_;
            transcriptStamp_=stamp; captionModel_=fileModel;
            if(captionModel_!=previousCaption) applyImeIcon();
            string next;
            if(exists&&matches&&!liveOwnCaption()){
                const string meta=SpeechService::read(resultPath(L".json"));
                const string original=imePanelUtf8(jsonField(meta,"original")), polished=imePanelUtf8(jsonField(meta,"polished"));
                if(!original.empty()||!polished.empty()){
                    if(!original.empty()) next+=wideToUtf8(uiText("speech.original"))+"\n"+original+"\n";
                    if(!polished.empty()) next+=wideToUtf8(uiText("speech.polished"))+"\n"+polished;
                } else next=imePanelUtf8(SpeechService::read(resultPath(L".txt")));
            }
            // WeChat's own voice bar carries the live caption; this page shows the reason instead of a copy.
            const wchar_t* placeholder=liveOwnCaption()?uiText("speech.ime_own_caption"):uiText(live_?"speech.live_empty":"speech.history_empty");
            if(next!=shownText_){
                shownText_=next;const LRESULT first=SendMessageW(viewer_,EM_GETFIRSTVISIBLELINE,0,0);
                SetWindowTextW(viewer_,next.empty()?placeholder:transcriptText().c_str());
                if(live_){const int length=GetWindowTextLengthW(viewer_);SendMessageW(viewer_,EM_SETSEL,length,length);SendMessageW(viewer_,EM_SCROLLCARET,0,0);}
                else SendMessageW(viewer_,EM_LINESCROLL,0,first);
            }else if(force&&next.empty()){
                const int length=GetWindowTextLengthW(viewer_);wstring current(static_cast<size_t>(length)+1,L'\0');
                GetWindowTextW(viewer_,&current[0],length+1);current.resize(static_cast<size_t>(length));
                if(current!=placeholder)SetWindowTextW(viewer_,placeholder);
            }
        }
        showImePlayback();
        const bool liveState=state.liveEnabled||state.recording;
        if(force||lastPaintRevision_!=state.revision||lastPaintPanel_!=panel||lastPaintLive_!=liveState||lastPaintError_!=localError_||lastPaintModel_!=captionModel_){lastPaintRevision_=state.revision;lastPaintPanel_=panel;lastPaintLive_=liveState;lastPaintError_=localError_;lastPaintModel_=captionModel_;InvalidateRect(window_,NULL,FALSE);}
    }
    /// Paint a compact readiness summary; lengthy setup facts stay in the inline diagnostics panel.
    void paint(HDC external=NULL) {
        PAINTSTRUCT ps{};HDC dc=external?external:BeginPaint(window_,&ps);RECT area{};GetClientRect(window_,&area);
        HBRUSH bg=CreateSolidBrush(RGB(246,248,252));FillRect(dc,&area,bg);DeleteObject(bg);
        const int w=width(),h=height();const auto state=g_speech.state();const bool ready=g_speech.readyFor(provider());
        surface(dc,12,8,236,h-24);surface(dc,260,8,w-272,h-24);
        const bool owns=state.busy&&state.operationModel==provider();
        const bool failed=!state.failureCode.empty()&&(state.operationModel.empty()||state.operationModel==provider());
        const LiveImeEntry* ime=imeSelected()?selectedIme():nullptr;
        const bool imePage=imeSelected();
        const wchar_t* imeStatus=nullptr;
        if(imePage){
            if(state.livePhase=="ime_wait") imeStatus=uiText("speech.ime_waiting");
            else if(state.livePhase=="ime_fallback") imeStatus=uiText("speech.ime_fallback");
            else if(state.livePhase=="ime_no_device") imeStatus=uiText("speech.ime_no_device");
            else if(!ime||!ime->supported) imeStatus=uiText("speech.ime_blocked");
            else imeStatus=uiText("speech.live_waiting");
        }
        // The cable state has its own line on both pages; this summary keeps task failures and phases.
        wstring status=localError_;
        if(status.empty()&&imeStatus) status=imeStatus;
        else if(status.empty()&&failed) status=uiText(("speech."+state.failureCode).c_str());
        else if(status.empty()&&owns) status=uiText(("speech."+state.step).c_str());
        else if(status.empty()&&!live_&&!state.historyEnabled) status=uiText("speech.history_off");
        else if(status.empty()&&!imePage) status=uiText(ready?"speech.module_ready":"speech.module_needed");
        text(dc,status,28,430,204,std::max(36,h-480),((!imeStatus&&failed)||localErrorFail_)?RGB(171,58,40):RGB(68,88,114),small_);
        if(owns){RECT track{px(28),px(414),px(232),px(418)};HBRUSH base=CreateSolidBrush(RGB(222,229,240));FillRect(dc,&track,base);DeleteObject(base);track.right=px(28+std::max(3,state.percent)*204/100);HBRUSH bar=CreateSolidBrush(RGB(43,105,230));FillRect(dc,&track,bar);DeleteObject(bar);}
        const wstring source=live_?sourceCaption():L"";
        wstring heading=uiText(advice_?"speech.hardware_button":details_?"speech.failure_details":live_?"speech.live_title":"speech.history_choose");
        if(live_&&!details_&&!advice_&&!source.empty()) heading+=L" · "+source;
        const bool mark=live_&&imeIcon_&&!details_&&!advice_;
        text(dc,heading,mark?308:280,20,w-(mark?336:308),26,RGB(31,51,77),font_,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(!live_&&!details_&&!advice_&&shownPlayTotalMs_) text(dc,playProgressText(),280,116,w-308,22,RGB(43,105,230),small_,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        else if(!live_&&!details_&&!advice_) text(dc,uiText("speech.history_view_note"),280,116,w-308,28,RGB(100,116,139),small_);
        if(live_&&!details_&&!advice_){
            const char* key=!state.liveEnabled?"speech.live_off":state.livePhase=="ime_wait"?"speech.ime_waiting":state.livePhase=="ime_fallback"?"speech.ime_fallback":state.livePhase=="ime_no_device"?"speech.ime_no_device":state.livePhase=="ime_unsupported"||(imePage&&ime&&!ime->supported)?"speech.ime_unsupported":state.recording?"speech.live_recording":"speech.live_waiting";
            text(dc,uiText(key),280,46,w-308,22,RGB(100,116,139),small_,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
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
        case WM_APP+3:self->imeFileRunning_=false;self->resultModel_=self->provider();if(HWND versions=GetDlgItem(window,34))SendMessageW(versions,CB_SETCURSEL,self->versionIndex(self->resultModel_),0);self->transcriptStamp_=0;self->localError_.clear();self->localErrorFail_=false;if(wparam==4)self->notice(uiText("speech.ime_no_device"),true);else if(wparam==8)self->notice(uiText("speech.ime_voice_mic"),true);else if(wparam==7)self->notice(uiText(self->provider()=="windows"?"speech.ime_voice_caret":"speech.ime_voice_closed"),true);else if(wparam==6)self->notice(uiText("speech.ime_voice_empty"),true);else if(wparam==0)self->notice(uiText("speech.ime_voice_opened"),false);else if(wparam!=0)self->localError_=uiText("speech.ime_waiting");self->refresh(true);return 0;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:{
            HDC dc=reinterpret_cast<HDC>(wparam);SetBkColor(dc,RGB(255,255,255));
            // The cable status line reads as a failure only while the endpoint is missing.
            const bool warn=message==WM_CTLCOLORSTATIC&&GetDlgCtrlID(reinterpret_cast<HWND>(lparam))==60&&!self->cablePresent_;
            SetTextColor(dc,warn?RGB(171,58,40):RGB(31,51,77));
            return reinterpret_cast<LRESULT>(self->white_);
        }
        case WM_DRAWITEM: {
            const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);const bool primary=item->CtlID==4||item->CtlID==32,disabled=item->itemState&ODS_DISABLED;
            HBRUSH fill=CreateSolidBrush(primary?disabled?RGB(222,229,240):RGB(43,105,230):RGB(255,255,255));FillRect(item->hDC,&item->rcItem,fill);DeleteObject(fill);
            HBRUSH edge=CreateSolidBrush(RGB(209,219,234));FrameRect(item->hDC,&item->rcItem,edge);DeleteObject(edge);
            wchar_t caption[256]{};GetWindowTextW(item->hwndItem,caption,256);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?RGB(143,155,171):primary?RGB(255,255,255):RGB(68,88,114));
            HGDIOBJ font=SelectObject(item->hDC,self->small_);RECT r=item->rcItem;DrawTextW(item->hDC,caption,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(item->hDC,font);if(item->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(item->hDC,&r);}return TRUE;
        }
        case WM_COMMAND: {
            const UINT notify=HIWORD(wparam);
            // Edit notifications are produced by our own text refresh. Handling them calls refresh again and overflows the UI stack.
            if(notify==EN_SETFOCUS||notify==EN_KILLFOCUS||notify==EN_CHANGE||notify==EN_UPDATE||notify==EN_ERRSPACE||notify==EN_MAXTEXT||notify==EN_HSCROLL||notify==EN_VSCROLL)return 0;
            const int id=LOWORD(wparam);const bool changed=notify==CBN_SELCHANGE;
            if(id==1||id==45||((id==2||id==3||id==8)&&changed))self->configure();
            if(id==8&&changed){self->showEngine();
                if(imeResultModel(self->provider())){self->details_=false;self->advice_=false;self->cablePoll_=5;}}
            if(id==51&&changed){g_speech.selectLiveEngine(SendMessageW(GetDlgItem(window,51),CB_GETCURSEL,0,0)==0?"ime":"sense");self->showEngine();}
            if((id==53||id==55)&&changed)self->applyImeSelection();
            if(id==61)g_speech.selectImeFocusPause(SendMessageW(GetDlgItem(window,61),BM_GETCHECK,0,0)==BST_CHECKED);
            if(id==4){
                if(!self->live_ && !g_speech.state().historyEnabled)self->notice(uiText("speech.history_off"),true);
                else if(self->configure()){if(self->live_ && !g_speech.state().enabled)g_speech.configure(true,g_speech.language(),g_speech.source(),true);g_speech.prepare(self->provider(),self->live_?g_speech.liveSource():g_speech.source());}
            }
            if(id==5){if(self->imeFileRunning_.load()||ImeFileSession::running().load())ImeFileSession::stop();else g_speech.cancelModel(self->provider());}
            if(id==9){self->details_=!self->details_;self->advice_=false;}
            if(id==12){self->advice_=!self->advice_;self->details_=false;}
            if(id==59){startCableInstall();self->cablePhase_=-1;self->cablePoll_=5;}
            if(id==36)self->recordings();
            if(id==35&&changed){const int i=static_cast<int>(SendMessageW(GetDlgItem(window,35),CB_GETCURSEL,0,0));if(i>=0&&i<static_cast<int>(self->files_.size()))self->wav_=self->files_[i];const string model=self->sharedCaptionModel();if(imeResultModel(model))self->resultModel_=model;else if(imeResultModel(self->resultModel_))self->resultModel_="sense";if(HWND versions=GetDlgItem(window,34))SendMessageW(versions,CB_SETCURSEL,self->versionIndex(self->resultModel_),0);self->transcriptStamp_=0;self->applyImeIcon();}
            if(id==34&&changed){const int i=static_cast<int>(SendMessageW(GetDlgItem(window,34),CB_GETCURSEL,0,0));self->resultModel_=self->versionModel(i);self->transcriptStamp_=0;self->applyImeIcon();}
            if(id==31)self->copyText(self->transcriptText());if(id==11)self->copyText(self->failureText(g_speech.state()));
            if(id==32&&self->configure()){
                const string model=self->provider();
                if(!g_speech.state().historyEnabled)self->notice(uiText("speech.history_off"),true);
                else if(imeResultModel(model)) self->startImeFile(model);
                else if(!g_speech.readyFor(model))self->notice(uiText("speech.module_needed"),true);
                else {self->resultModel_=model;SendMessageW(GetDlgItem(window,34),CB_SETCURSEL,self->versionIndex(model),0);if(!g_speech.enqueue(self->wav_,false,model))self->notice(uiText("speech.admission_failed"),true);else{self->localError_.clear();self->localErrorFail_=false;}}
            }
            self->refresh(true);return 0;
        }
        case WM_NCDESTROY:KillTimer(window,1);if(self->imeIcon_)DestroyIcon(self->imeIcon_);if(self->tooltip_)DestroyWindow(self->tooltip_);instance()=NULL;SetWindowLongPtrW(window,GWLP_USERDATA,0);DeleteObject(self->font_);DeleteObject(self->small_);DeleteObject(self->white_);delete self;return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    /// Native controls and brushes belong to this one UI child.
    HWND window_=NULL,enabled_=NULL,language_=NULL,source_=NULL,install_=NULL,cancel_=NULL,viewer_=NULL,detail_=NULL;
    HFONT font_=NULL,small_=NULL;HBRUSH white_=NULL;
    /// Task identity, inline auxiliary panel and persistent file-version selection.
    bool live_=false,savedLive_=false,details_=false,advice_=false;
    int dpi_=96,locale_=-1,laidWidth_=0,laidHeight_=0;unsigned ramGiB_=0,cpuThreads_=0;
    uint64_t revision_=static_cast<uint64_t>(-1),failureRevision_=0,transcriptStamp_=0,lastPaintRevision_=static_cast<uint64_t>(-1),shownPlaySec_=0,shownPlayTotalMs_=0;
    /// Last painted summary, so the 200ms poll does not redraw both speech pages when nothing changed.
    bool lastPaintPanel_=false,lastPaintLive_=false;
    wstring wav_,shownDetail_,localError_,lastPaintError_,tooltipText_,supportedNames_;string shownText_,resultModel_="sense",captionModel_,lastPaintModel_,lastFailure_;
    /// Inline messages that report a failed action are painted in the warning colour instead of the neutral one.
    bool localErrorFail_=false;
    /// True while the transcription keeps playing behind another window.
    bool away_=false;
    /// Virtual cable availability and the installer phase shown next to the input-method choices.
    bool cablePresent_=true; int cablePhase_=-1; int cablePoll_=0; wstring cableButton_,cableStatus_;
    HICON imeIcon_=NULL; HWND tooltip_=NULL;
    std::vector<wstring> files_;
    std::vector<string> historyModels_;
    std::atomic<bool> imeFileRunning_{false};
    std::vector<LiveImeEntry> imes_;
    std::vector<LivePlaybackDevice> devices_;
    int imePoll_=0;
    const char* languages_[6]={"auto","zh","en","yue","ja","ko"};
};

/// Screen-corner captions while a call is recording. Finished segments stay in the recording's transcript file.
class LiveCaptionOverlay {
public:
    /// Show the latest saved text without activating or navigating the main window.
    static void tick(HWND owner, int dpi) {
        const auto state = g_speech.state();
        const bool active = owner && state.liveEnabled && state.recording && !state.liveWav.empty();
        // WeChat draws its own live caption, so a second corner box would only duplicate it.
        if (!active || ownCaptionIme()) { if (window_) ShowWindow(window_, SW_HIDE); return; }
        if (wav_ != state.liveWav) { wav_ = state.liveWav; stamp_ = 0; text_.clear(); if (edit_) SetWindowTextW(edit_, uiText("speech.live_empty")); }
        ensure(dpi);
        const wstring file = wav_ + L".transcript.txt";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        uint64_t stamp = 0;
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info))
            stamp = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        if (stamp != stamp_) {
            stamp_ = stamp;
            wstring next = utf8ToWide(stamp ? SpeechService::read(file, 256 * 1024) : "");
            if (next.size() > 8000) next.erase(0, next.size() - 8000);
            if (next != text_) {
                text_ = std::move(next);
                SetWindowTextW(edit_, text_.empty() ? uiText("speech.live_empty") : text_.c_str());
                const int length = GetWindowTextLengthW(edit_);
                SendMessageW(edit_, EM_SETSEL, length, length);
                SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
            }
        }
        place(owner, dpi);
    }
    /// Remove the corner window when the desktop window closes.
    static void close() {
        if (window_) DestroyWindow(window_);
        window_ = edit_ = title_ = icon_ = NULL; wav_.clear(); text_.clear(); stamp_ = 0;
        if (font_) { DeleteObject(font_); font_ = NULL; }
        if (brush_) { DeleteObject(brush_); brush_ = NULL; }
    }
private:
    /// True while the selected live engine is the input method that shows its own caption.
    static bool ownCaptionIme() {
        if (g_speech.liveEngine() != "ime") return false;
        string tip, profile, device;
        g_speech.liveImeSelection(tip, profile, device);
        const char* model = supportedImeModel(tip, profile);
        return model && string(model) == "wetype";
    }
    /// Paint the card without taking keyboard focus.
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(wparam), message == WM_CTLCOLORSTATIC ? RGB(15, 23, 42) : RGB(30, 41, 59));
            return reinterpret_cast<LRESULT>(brush_);
        }
        if (message == WM_DESTROY) { window_ = edit_ = title_ = icon_ = NULL; return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    /// Create the topmost tool window once, and refresh its font when DPI changes.
    static void ensure(int dpi) {
        if (window_ && dpi_ == dpi) return;
        if (!window_) {
            WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(NULL);
            type.lpszClassName = L"PhoneRecordLiveCaption"; type.hCursor = LoadCursorW(NULL, IDC_ARROW);
            RegisterClassExW(&type); brush_ = CreateSolidBrush(RGB(255, 255, 255));
            window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"PhoneRecord", WS_POPUP | WS_BORDER, 0, 0, 0, 0, NULL, NULL, type.hInstance, NULL);
            const int iconPixels = GetSystemMetrics(SM_CXSMICON);
            HICON icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_PHONERECORD), IMAGE_ICON, iconPixels, iconPixels, LR_SHARED));
            if (!icon) icon = LoadIconW(NULL, IDI_APPLICATION);
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_PHONERECORD), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED)));
            icon_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ICON, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(3), type.hInstance, NULL);
            SendMessageW(icon_, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(icon));
            title_ = CreateWindowExW(0, L"STATIC", (L"PhoneRecord  ·  " + wstring(uiText("speech.module_live"))).c_str(), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(1), type.hInstance, NULL);
            edit_ = CreateWindowExW(0, L"EDIT", uiText("speech.live_empty"), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(2), type.hInstance, NULL);
        }
        if (font_) DeleteObject(font_);
        dpi_ = dpi;
        font_ = CreateFontW(-MulDiv(15, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    /// Keep the box at the working area's lower-right corner on the main window's monitor.
    static void place(HWND owner, int dpi) {
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(monitor, &info)) return;
        const int width = MulDiv(420, dpi, 96), height = MulDiv(168, dpi, 96), margin = MulDiv(20, dpi, 96), pad = MulDiv(12, dpi, 96), titleHeight = MulDiv(28, dpi, 96);
        const int iconSize = GetSystemMetrics(SM_CXSMICON);
        SetWindowPos(window_, HWND_TOPMOST, info.rcWork.right - width - margin, info.rcWork.bottom - height - margin, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        MoveWindow(icon_, pad, pad + (titleHeight - iconSize) / 2, iconSize, iconSize, TRUE);
        MoveWindow(title_, pad + iconSize + MulDiv(8, dpi, 96), pad, width - pad * 2 - iconSize - MulDiv(8, dpi, 96), titleHeight, TRUE);
        MoveWindow(edit_, pad, pad + titleHeight, width - pad * 2 - GetSystemMetrics(SM_CXVSCROLL), height - pad * 2 - titleHeight, TRUE);
    }
    static inline HWND window_ = NULL, edit_ = NULL, title_ = NULL, icon_ = NULL;
    static inline HFONT font_ = NULL;
    static inline HBRUSH brush_ = NULL;
    static inline int dpi_ = 0;
    static inline uint64_t stamp_ = 0;
    static inline wstring wav_, text_;
};
#endif
