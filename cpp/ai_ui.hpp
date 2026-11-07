#pragma once
#ifdef _WIN64

/// Native embedded page for independent CLI/project/template profiles and per-call analysis.
class AiPage {
public:
    /// Reuse the page while navigating; a new call replaces only call-specific selections.
    static HWND show(HWND parent,const wstring& wav,int dpi) {
        if(instance()) {
            auto* self=reinterpret_cast<AiPage*>(GetWindowLongPtrW(instance(),GWLP_USERDATA));
            if(!wav.empty() && self->wav_!=wav) {self->wav_=wav;self->mode_=0;self->build();}
            return instance();
        }
        auto* self=new AiPage; self->wav_=wav; self->dpi_=dpi;
        WNDCLASSEXW type{};type.cbSize=sizeof(type);type.hInstance=GetModuleHandleW(NULL);type.lpfnWndProc=procedure;
        type.lpszClassName=L"PhoneRecordAiPage";type.hCursor=LoadCursorW(NULL,IDC_ARROW);RegisterClassExW(&type);
        HWND child=CreateWindowExW(WS_EX_CONTROLPARENT,type.lpszClassName,L"",WS_CHILD|WS_CLIPCHILDREN,0,0,1,1,parent,reinterpret_cast<HMENU>(961),type.hInstance,self);
        if(!child) {delete self;return NULL;} instance()=child;return child;
    }
    /// Stable handle owned by the desktop shell's page seven.
    static HWND& instance() {static HWND value=NULL;return value;}
    /// Resize the page and recreate its font only after a genuine DPI change.
    static void place(int x,int y,int w,int h,int dpi) {
        if(!instance()) return;
        auto* self=reinterpret_cast<AiPage*>(GetWindowLongPtrW(instance(),GWLP_USERDATA));
        if(self->dpi_!=dpi) {self->dpi_=dpi;self->font();}
        MoveWindow(instance(),MulDiv(x,dpi,96),MulDiv(y,dpi,96),MulDiv(w,dpi,96),MulDiv(h,dpi,96),TRUE);
    }
private:
    /// Scale design coordinates to the parent shell's monitor.
    int px(int value) const {return MulDiv(value,dpi_,96);}
    /// Read one bounded native edit value as UTF-8 without copying private text to logs.
    string value(int id) const {
        HWND edit=GetDlgItem(window_,id);int length=GetWindowTextLengthW(edit);wstring text(static_cast<size_t>(length)+1,L'\0');
        GetWindowTextW(edit,&text[0],length+1);text.resize(length);return wideToUtf8(text);
    }
    /// Replace Windows edit text while preserving the persisted UTF-8 contract.
    void set(int id,const string& text) {SetWindowTextW(GetDlgItem(window_,id),utf8ToWide(text).c_str());}
    /// Convert persisted LF content for readable native multiline controls.
    static wstring display(const string& text) {wstring out;for(wchar_t ch:utf8ToWide(text)){if(ch==L'\n' && (out.empty() || out.back()!=L'\r'))out+=L'\r';out+=ch;}return out;}
    /// Create one accessible native control; only product copy receives a resource key.
    HWND control(const wchar_t* kind,int id,const char* key,DWORD style=0) {
        HWND child=CreateWindowExW(wcscmp(kind,L"EDIT")==0?WS_EX_CLIENTEDGE:0,kind,key?uiText(key):L"",WS_CHILD|WS_VISIBLE|style,0,0,0,0,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(NULL),NULL);
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        if(key) labels_[id]=key;
        if(wcscmp(kind,L"EDIT")==0) SendMessageW(child,EM_SETLIMITTEXT,id==80?2*1024*1024:32768,0);
        return child;
    }
    /// Create a keyboard-accessible action with ordinary native focus behavior.
    void button(int id,const char* key) {control(L"BUTTON",id,key,WS_TABSTOP|BS_OWNERDRAW);}
    /// Create an editable field, optionally multiline with its own scroll range.
    void edit(int id,bool multi=false) {control(L"EDIT",id,nullptr,WS_TABSTOP|(multi?ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL:ES_AUTOHSCROLL));}
    /// Create a dropdown whose item identities are kept separately from display names.
    void combo(int id) {control(WC_COMBOBOXW,id,nullptr,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);}
    /// Recreate the owned GUI font and apply it to existing child controls.
    void font() {
        HFONT old=font_;font_=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(window_) for(HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        if(old)DeleteObject(old);
    }
    /// Append a localized name with its stable profile identity.
    void item(int id,const string& identity,const wstring& name) {SendMessageW(GetDlgItem(window_,id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));ids_[id].push_back(identity);}
    /// Resolve the current combo identity; empty selection is a legitimate project-free analysis.
    string selected(int id) const {
        const int index=static_cast<int>(SendMessageW(GetDlgItem(window_,id),CB_GETCURSEL,0,0));auto found=ids_.find(id);
        return found!=ids_.end() && index>=0 && index<static_cast<int>(found->second.size())?found->second[index]:"";
    }
    /// Select by stable identity rather than translated display text.
    void select(int id,const string& identity) {
        const auto& values=ids_[id];auto found=std::find(values.begin(),values.end(),identity);
        SendMessageW(GetDlgItem(window_,id),CB_SETCURSEL,found==values.end()?0:static_cast<WPARAM>(found-values.begin()),0);
    }
    /// Populate a category and optional built-in prompt contracts.
    void populate(int id,const string& kind,bool empty=true,bool builtins=false) {
        SendMessageW(GetDlgItem(window_,id),CB_RESETCONTENT,0,0);ids_[id].clear();
        if(empty)item(id,"",uiText("ai.none"));
        if(builtins) for(const char* key:{"summary","requirements","diagnosis","tasks"})item(id,key,uiText((string("ai.template.")+key).c_str()));
        for(const auto& p:g_ai.profiles())if(p.kind==kind)item(id,p.id,utf8ToWide(p.name));
        SendMessageW(GetDlgItem(window_,id),CB_SETCURSEL,0,0);
    }
    /// Return the selected saved profile or the selected built-in prompt snapshot.
    AiProfile profile(int id) const {
        const string key=selected(id);if(const auto* p=g_ai.find(key))return *p;
        if(id!=32) return AiProfile{};
        AiProfile p;p.id=key;p.kind="prompt";p.body=wideToUtf8(uiText(("ai.instructions."+key).c_str()));return p;
    }
    /// Build one tab. Switching settings never starts or cancels a background analysis.
    void build() {
        for(HWND child=GetWindow(window_,GW_CHILD);child;) {HWND next=GetWindow(child,GW_HWNDNEXT);DestroyWindow(child);child=next;}
        labels_.clear();ids_.clear();shown_.clear();revision_=static_cast<uint64_t>(-1);
        control(L"BUTTON",15,"ai.enable",WS_TABSTOP|BS_AUTOCHECKBOX);
        SendMessageW(GetDlgItem(window_,15),BM_SETCHECK,g_ai.enabled()?BST_CHECKED:BST_UNCHECKED,0);
        button(10,"ai.tab.analysis");button(11,"ai.tab.cli");button(12,"ai.tab.projects");button(13,"ai.tab.prompts");
        if(mode_==0) {
            control(L"STATIC",100,"ai.project");combo(30);button(50,"ai.bind");
            control(L"STATIC",101,"ai.cli");combo(31);control(L"STATIC",102,"ai.prompt");combo(32);
            control(L"BUTTON",40,"ai.source.sense",WS_TABSTOP|BS_AUTOCHECKBOX);control(L"BUTTON",41,"ai.source.qwen",WS_TABSTOP|BS_AUTOCHECKBOX);control(L"BUTTON",42,"ai.source.qwen17",WS_TABSTOP|BS_AUTOCHECKBOX);
            SendMessageW(GetDlgItem(window_,40),BM_SETCHECK,BST_CHECKED,0);SendMessageW(GetDlgItem(window_,41),BM_SETCHECK,BST_CHECKED,0);
            combo(33);item(33,"zh-CN",L"简体中文");item(33,"zh-TW",L"繁體中文");item(33,"en",L"English");select(33,uiLanguageCode());
            control(L"STATIC",103,"ai.extra");edit(34,true);
            button(51,"ai.analyze");button(52,"ai.preview");button(53,"ai.cancel");button(54,"ai.copy");button(55,"ai.export");button(56,"ai.latest");button(58,"ai.history");
            control(L"STATIC",90,nullptr);edit(80,true);SendMessageW(GetDlgItem(window_,80),EM_SETREADONLY,TRUE,0);
            combo(35);
            populate(30,"project");populate(31,"cli");populate(32,"prompt",false,true);
            if(!wav_.empty()) {const string bound=g_ai.binding(wav_);select(30,bound);unresolved_=!bound.empty()&&!g_ai.find(bound);defaults();latest();}
            preferCli();
        } else {
            control(L"STATIC",110,"ai.saved_profile");combo(20);button(21,"ai.new");button(22,"ai.save");button(23,"ai.delete");control(L"STATIC",90,nullptr);
            control(L"STATIC",100,"ai.name");edit(60);
            if(mode_==1) {
                control(L"LISTBOX",24,nullptr,WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL|WS_BORDER);catalog();
                control(L"STATIC",101,"ai.type");combo(61);for(const auto& tool:kAiCliTools)item(61,tool.id,tool.name);item(61,"stdin",uiText("ai.type.stdin"));select(61,"claude");
                control(L"STATIC",102,"ai.executable");edit(62);button(71,"ai.browse");
                control(L"STATIC",103,"ai.model");control(WC_COMBOBOXW,63,nullptr,WS_TABSTOP|CBS_DROPDOWN|WS_VSCROLL);control(L"STATIC",106,"ai.effort");combo(66);item(66,"",uiText("ai.inherit"));for(const char* effort:{"minimal","low","medium","high","xhigh","max"})item(66,effort,utf8ToWide(effort));select(66,"");control(L"STATIC",104,"ai.timeout_seconds");edit(64);set(64,"300");button(57,"ai.test");                control(L"STATIC",105,"ai.lifecycle_help");
                button(72,"ai.install_cli");button(73,"ai.refresh_models");button(74,"ai.login_cli");button(77,"ai.custom_cli");button(76,"ai.official_site");control(L"STATIC",108,nullptr);button(53,"ai.cancel");
                ShowWindow(GetDlgItem(window_,61),SW_HIDE);ShowWindow(GetDlgItem(window_,101),SW_HIDE);
            } else if(mode_==2) {
                control(L"STATIC",102,"ai.folder");edit(62);button(71,"ai.browse");
                control(L"STATIC",101,"ai.default_cli");combo(61);populate(61,"cli");
                control(L"STATIC",103,"ai.default_prompt");combo(63);populate(63,"prompt",false,true);select(63,"requirements");
                control(L"STATIC",104,"ai.project_prompt");edit(64,true);control(L"STATIC",105,"ai.references");edit(65,true);
            } else {control(L"STATIC",104,"ai.prompt_body");edit(64,true);}
            populate(20,mode_==1?"cli":mode_==2?"project":"prompt",false,mode_==3);load();
            button(59,"ai.show_final");
        }
        locale_=g_uiLanguage.load();layout();refresh();
    }
    /// Leave “none” only when there is no saved CLI; a finished result prefers the profile that produced it.
    void preferCli() {
        if(mode_!=0 || !selected(31).empty() || ids_[31].size()<2) return;
        if(!resultFiles_.empty()) {
            const wstring meta=resultFiles_.front();
            const auto slash=meta.find_last_of(L"\\/");
            const wstring dir=slash==wstring::npos?L"":meta.substr(0,slash);
            const string name=jsonField(SpeechService::read(dir+L"\\analysis.json",256*1024),"cliName");
            if(!name.empty()) for(const string& id:ids_[31]) if(const auto* p=g_ai.find(id)) if(p->name==name) {select(31,id);return;}
        }
        select(31,ids_[31][1]);
    }
    /// Highlight the installed-tool row that matches the profile being edited.
    void syncToolList() {
        HWND list=GetDlgItem(window_,24);if(!list) return;
        const string type=selected(61);
        for(int index=0;index<static_cast<int>(std::size(kAiCliTools));++index) if(kAiCliTools[index].id==type) {SendMessageW(list,LB_SETCURSEL,index,0);return;}
        SendMessageW(list,LB_SETCURSEL,static_cast<WPARAM>(-1),0);
    }
    /// Apply project defaults only when the user selects a project, keeping one-off overrides local.
    void defaults() {
        if(const auto* p=g_ai.find(selected(30))) {select(31,p->cli);select(32,p->prompt);}
    }
    /// Display one saved profile or a built-in template ready to be copied as a custom profile.
    void load() {
        const auto* p=g_ai.find(selected(20));editing_=p?p->id:"";
        if(!p) {
            if(mode_==3) {set(60,wideToUtf8(uiText(("ai.template."+selected(20)).c_str())));SetWindowTextW(GetDlgItem(window_,64),uiText(("ai.instructions."+selected(20)).c_str()));}
            return;
        }
        set(60,p->name);set(62,p->path);
        if(mode_==1) {select(61,p->type);set(63,p->model);modelChoices(p->model);select(66,p->effort);set(64,std::to_string(p->timeout));}
        else {select(61,p->cli);select(63,p->prompt);SetWindowTextW(GetDlgItem(window_,64),display(p->body).c_str());SetWindowTextW(GetDlgItem(window_,65),display(p->references).c_str());}
        if(mode_==1) syncToolList();
    }
    /// Read the active editor without mutating the persisted profile collection.
    AiProfile editor() const {
        AiProfile p;p.id=editing_;p.kind=mode_==1?"cli":mode_==2?"project":"prompt";p.name=value(60);p.path=value(62);
        if(mode_==1) {p.type=selected(61);p.model=value(63);p.effort=selected(66);p.timeout=static_cast<unsigned>(strtoul(value(64).c_str(),NULL,10));}
        else {p.cli=selected(61);p.prompt=selected(63);p.body=value(64);p.references=value(65);}
        return p;
    }
    /// Capture the call, independent source choices, project and per-operation overrides.
    AiRequest request() const {
        AiRequest r;r.wav=wav_;r.cli=profile(31);r.project=profile(30);r.prompt=profile(32);r.extra=value(34);r.language=selected(33);
        r.sense=SendMessageW(GetDlgItem(window_,40),BM_GETCHECK,0,0)==BST_CHECKED;
        r.qwen=SendMessageW(GetDlgItem(window_,41),BM_GETCHECK,0,0)==BST_CHECKED;r.qwen17=SendMessageW(GetDlgItem(window_,42),BM_GETCHECK,0,0)==BST_CHECKED;return r;
    }
    /// List the curated tools with locally observed installation state; no network calls on redraw.
    void catalog() {
        HWND list=GetDlgItem(window_,24);if(!list)return;
        const int previous=static_cast<int>(SendMessageW(list,LB_GETCURSEL,0,0));SendMessageW(list,LB_RESETCONTENT,0,0);
        for(const auto& tool:kAiCliTools) {
            const auto state=g_ai.observation(tool.id);const bool installed=!aiCliPath(tool.id).empty() || (!state.path.empty() && !aiCliPath(tool.id,state.path).empty());
            const wstring label=wstring(installed?L"● ":L"○ ")+(string(tool.id)=="codex"?L"ChatGPT / Codex":tool.name)+L" · "+uiText(installed?"ai.installed":"ai.not_installed");SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(list,LB_SETCURSEL,previous<0?0:previous,0);
    }
    /// Read the selected installed CLI's model list once; install completion and explicit selection both use this path.
    void ensureModels(bool afterInstall) {
        if(mode_!=1 || !g_ai.enabled())return;
        const auto state=g_ai.state();
        if(state.busy)return;
        if(afterInstall && (state.step!="installed" || modelsAfterInstall_==state.revision))return;
        if(afterInstall)modelsAfterInstall_=state.revision;
        const AiProfile profile=editor();
        if(!aiCliTool(profile.type) || aiCliPath(profile.type,profile.path).empty() || !g_ai.observation(profile.type).models.empty())return;
        if(HWND combo=GetDlgItem(window_,63))SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
        g_ai.manage(profile,"models");
    }
    /// Fill the preset-model dropdown from the fetched CLI list without a second copy of the same IDs.
    void modelChoices(const string& current="") {
        HWND combo=GetDlgItem(window_,63);if(!combo || mode_!=1)return;
        const string keep=current.empty()?value(63):current;const auto observed=g_ai.observation(selected(61));
        const bool open=SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)!=0;
        SendMessageW(combo,CB_RESETCONTENT,0,0);std::vector<string> seen;std::istringstream lines(observed.models);string line;
        while(std::getline(lines,line)){if(!line.empty() && line.back()=='\r')line.pop_back();if(line.empty() || line.size()>256 || std::find(seen.begin(),seen.end(),line)!=seen.end())continue;seen.push_back(line);SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(utf8ToWide(line).c_str()));}
        if(!keep.empty() && std::find(seen.begin(),seen.end(),keep)==seen.end() && keep.size()<=256)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(utf8ToWide(keep).c_str()));
        set(63,keep);const string source=observed.source.empty()?"manual":observed.source;
        wstring caption=uiText(("ai.models_source."+source).c_str());if(observed.cached)caption+=L"\r\n"+wstring(uiText("ai.models_cached"));SetWindowTextW(GetDlgItem(window_,108),caption.c_str());
        if(open)SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
    }
    /// Enumerate successful per-CLI final responses without mixing intermediate process output.
    void results(const wstring& preferred=L"") {
        if(wav_.empty() || mode_!=0)return;
        HWND combo=GetDlgItem(window_,35);SendMessageW(combo,CB_RESETCONTENT,0,0);resultFiles_.clear();
        try {
            namespace fs=std::filesystem;const fs::path root=wav_+L".ai-history";
            if(!fs::is_directory(root) || (GetFileAttributesW(root.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))return;
            std::vector<fs::path> directories;size_t visited=0;
            for(const auto& entry:fs::directory_iterator(root)) {if(++visited>1000)break;if(entry.is_directory() && !(GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT))directories.push_back(entry.path());}
            std::sort(directories.begin(),directories.end(),std::greater<fs::path>());
            for(const auto& dir:directories) {
                string meta=SpeechService::read((dir/L"analysis.json").native(),256*1024);if(meta.empty())meta=SpeechService::read((dir/L"request.json").native(),256*1024);
                if(jsonField(meta,"status")!="complete")continue;
                const auto file=dir/L"result.md";if(!fs::is_regular_file(file) || (GetFileAttributesW(file.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))continue;
                string name=jsonField(meta,"cliName");if(name.empty())name=jsonField(meta,"name");
                const wstring label=utf8ToWide(name+" / "+jsonField(meta,"model"))+L" · "+dir.filename().native();
                SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));resultFiles_.push_back(file.native());if(resultFiles_.size()>=100)break;
            }
            auto found=std::find(resultFiles_.begin(),resultFiles_.end(),preferred);SendMessageW(combo,CB_SETCURSEL,found==resultFiles_.end()?0:found-resultFiles_.begin(),0);
        }catch(...){}
    }
    /// Render a bounded local result only; never open a path outside this call's analysis history.
    void latest() {
        if(wav_.empty())return;
        const wstring result=utf8ToWide(jsonField(SpeechService::read(wav_+L".ai-latest.json",32768),"result"));
        const wstring root=wav_+L".ai-history\\";
        if(result.compare(0,root.size(),root)==0 && result.find(L"..",root.size())==wstring::npos) showResult(result);
        results(result);
    }
    /// Update the multiline viewer with LF conversion and keep the exact export bytes separately.
    void showResult(const wstring& file) {shown_=SpeechService::read(file,4*1024*1024);SetWindowTextW(GetDlgItem(window_,80),display(shown_).c_str());}
    /// Copy only the currently displayed prompt/result after an explicit button press.
    void copy() {
        const wstring text=display(shown_);if(!OpenClipboard(window_))return;
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
        if(memory) {void* data=GlobalLock(memory);if(data){memcpy(data,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}else GlobalFree(memory);}CloseClipboard();
    }
    /// Export visible content to the user-selected Markdown file without altering the project.
    void exportResult() {
        wchar_t path[32768]=L"call-analysis.md";OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window_;dialog.lpstrFile=path;dialog.nMaxFile=32768;
        dialog.lpstrFilter=L"Markdown (*.md)\0*.md\0\0";dialog.lpstrDefExt=L"md";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(GetSaveFileNameW(&dialog) && !SpeechService::write(path,shown_))notice("ai.save_failed");
    }
    /// Select an executable or filesystem folder without accepting arbitrary shell commands.
    void browse() {
        if(mode_==1) {
            wchar_t path[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window_;dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrFilter=L"Executable (*.exe)\0*.exe\0\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&dialog))set(62,wideToUtf8(path));
        } else {
            BROWSEINFOW info{};info.hwndOwner=window_;info.lpszTitle=uiText("ai.folder");info.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE result=SHBrowseForFolderW(&info);if(result){wchar_t path[MAX_PATH]{};if(SHGetPathFromIDListW(result,path))set(62,wideToUtf8(path));CoTaskMemFree(result);}
        }
    }
    /// Show localized validation feedback without emitting private provider output.
    void notice(const char* key) {MessageBoxW(window_,uiText(key),uiText("ai.title"),MB_OK|MB_ICONINFORMATION);}
    /// Dispatch only explicit actions; switching tabs or projects never sends content externally.
    void command(int id,int notification) {
        if(id==15){if(!g_ai.setEnabled(SendMessageW(GetDlgItem(window_,15),BM_GETCHECK,0,0)==BST_CHECKED))notice("ai.save_failed");}
        if((id==57||id==72||id==73||id==74) && !g_ai.enabled()){if(!g_ai.setEnabled(true)){notice("ai.save_failed");refresh();return;}SendMessageW(GetDlgItem(window_,15),BM_SETCHECK,BST_CHECKED,0);}
        if(id==51 && !g_ai.enabled()){notice("ai.disabled");refresh();return;}
        if(id>=10 && id<=13) {mode_=id-10;build();return;}
        if(id==20 && notification==CBN_SELCHANGE && static_cast<int>(SendMessageW(GetDlgItem(window_,20),CB_GETCURSEL,0,0))>=0) load();
        if(id==24 && notification==LBN_SELCHANGE) {
            const int index=static_cast<int>(SendMessageW(GetDlgItem(window_,24),LB_GETCURSEL,0,0));
            if(index>=0 && index<static_cast<int>(std::size(kAiCliTools))){const auto& tool=kAiCliTools[index];editing_.clear();set(60,wideToUtf8(wstring(tool.name)));select(61,tool.id);set(62,wideToUtf8(aiCliPath(tool.id)));set(63,"");modelChoices();ensureModels(false);SendMessageW(GetDlgItem(window_,20),CB_SETCURSEL,static_cast<WPARAM>(-1),0);}
        }
        if(id==61 && notification==CBN_SELCHANGE){set(62,wideToUtf8(aiCliPath(selected(61))));set(63,"");modelChoices();ensureModels(false);}
        if(id==35 && notification==CBN_SELCHANGE){const int index=static_cast<int>(SendMessageW(GetDlgItem(window_,35),CB_GETCURSEL,0,0));if(index>=0 && index<static_cast<int>(resultFiles_.size()))showResult(resultFiles_[index]);}
        if(id==59){mode_=0;build();return;}
        if(id==30 && notification==CBN_SELCHANGE) {unresolved_=false;defaults();}
        if(id==21) {editing_.clear();for(int field:{60,62,63,64,65})set(field,"");if(mode_==1){set(64,"300");select(66,"");modelChoices();syncToolList();}SendMessageW(GetDlgItem(window_,20),CB_SETCURSEL,static_cast<WPARAM>(-1),0);}
        if(id==77) {editing_.clear();select(61,"stdin");set(60,wideToUtf8(uiText("ai.type.stdin")));set(62,"");set(63,"");set(64,"300");select(66,"");modelChoices();syncToolList();SendMessageW(GetDlgItem(window_,20),CB_SETCURSEL,static_cast<WPARAM>(-1),0);}
        if(id==22) {auto p=editor();if(p.id.empty())p.id=AiAnalysis::identity();if(!g_ai.save(p)){notice("ai.profile_invalid");return;}const string identity=p.id;populate(20,p.kind,false,mode_==3);select(20,identity);load();notice("ai.saved");}
        if(id==23 && !editing_.empty()) {if(!g_ai.erase(editing_)){notice("ai.referenced");return;}editing_.clear();build();}
        if(id==71)browse();
        if(id==72 || id==73){auto p=editor();if(!g_ai.manage(p,id==72?"install":"models"))notice("ai.busy");}
        if(id==74 && !g_ai.login(editor()))notice("ai.login_failed");
        if(id==76){if(const auto* tool=aiCliTool(selected(61)))ShellExecuteW(window_,L"open",tool->documentation,NULL,NULL,SW_SHOWNORMAL);}
        if(id==50) {if(!g_ai.bind(wav_,selected(30)))notice("ai.recording_invalid");else {unresolved_=false;notice("ai.saved");}}
        if(id==51 || id==52) {
            AiRequest r=request();if(wav_.empty() || (id==51 && r.cli.id.empty())){notice("ai.select_required");return;}
            if(!g_ai.bind(wav_,selected(30))) {notice("ai.recording_invalid");return;}
            unresolved_=false;if(!g_ai.run(r,id==52))notice("ai.busy");
        }
        if(id==57) {AiRequest r;r.test=true;r.cli=editor();if(r.cli.timeout<10 || r.cli.timeout>3600){notice("ai.profile_invalid");return;}if(!g_ai.run(r))notice("ai.busy");}
        if(id==53)g_ai.cancel();if(id==54)copy();if(id==55)exportResult();if(id==56)latest();
        if(id==58 && !wav_.empty()) {const wstring folder=wav_+L".ai-history";if(GetFileAttributesW(folder.c_str())!=INVALID_FILE_ATTRIBUTES)ShellExecuteW(window_,L"open",folder.c_str(),NULL,NULL,SW_SHOWNORMAL);}
        refresh();
    }
    /// Refresh state and localized labels without resetting edits or combo identities.
    void refresh() {
        g_ai.pollLogin();
        if(locale_!=g_uiLanguage.load()) {
            locale_=g_uiLanguage.load();revision_=static_cast<uint64_t>(-1);
            for(const auto& label:labels_)SetWindowTextW(GetDlgItem(window_,label.first),uiText(label.second.c_str()));
            for(const auto& combo:ids_) {
                const string previous=selected(combo.first);
                for(size_t index=0;index<combo.second.size();++index) {
                    const string& id=combo.second[index];const char* key=id.empty()?(combo.first==66?"ai.inherit":"ai.none"):id=="stdin"?"ai.type.stdin":nullptr;
                    string temporary;if(id=="summary"||id=="requirements"||id=="diagnosis"||id=="tasks"){temporary="ai.template."+id;key=temporary.c_str();}
                    if(key){SendMessageW(GetDlgItem(window_,combo.first),CB_DELETESTRING,index,0);SendMessageW(GetDlgItem(window_,combo.first),CB_INSERTSTRING,index,reinterpret_cast<LPARAM>(uiText(key)));}
                }
                select(combo.first,previous);
            }
        }
        const auto state=g_ai.state();
        const bool on=g_ai.enabled();
        if(enabledShown_!=on){enabledShown_=on;revision_=static_cast<uint64_t>(-1);}
        EnableWindow(GetDlgItem(window_,51),on&&!state.busy);
        EnableWindow(GetDlgItem(window_,57),!state.busy);
        EnableWindow(GetDlgItem(window_,52),!state.busy);
        for(int id:{72,73,74})EnableWindow(GetDlgItem(window_,id),!state.busy && selected(61)!="stdin");
        if(mode_==1){const auto observed=g_ai.observation(selected(61));if(toolRevision_!=observed.revision){toolRevision_=observed.revision;catalog();if(value(62).empty() && !observed.path.empty())set(62,observed.path);modelChoices();}ensureModels(true);}
        EnableWindow(GetDlgItem(window_,53),state.busy);
        if(revision_!=state.revision) {
            revision_=state.revision;
            wstring text=on?uiText(("ai."+state.step).c_str()):uiText("ai.disabled");if(on && !state.code.empty())text+=L" ("+utf8ToWide(state.code)+L")";
            if(on && mode_==0 && wav_.empty())text=uiText("ai.select_call");
            else if(on && mode_==0 && unresolved_)text=uiText("ai.project_missing");
            SetWindowTextW(GetDlgItem(window_,90),text.c_str());
            if(mode_==0 && !state.result.empty() && state.wav==wav_){showResult(state.result);if(state.step=="complete")results(state.result);}
        }
    }
    /// Reflow two-column native controls at the existing desktop minimum size.
    void layout() {
        RECT rect{};GetClientRect(window_,&rect);const int w=MulDiv(rect.right,96,dpi_),h=MulDiv(rect.bottom,96,dpi_),half=(w-44)/2;
        const auto move=[&](int id,int x,int y,int width,int height){if(HWND child=GetDlgItem(window_,id))MoveWindow(child,px(x),px(y),px(std::max(1,width)),px(std::max(1,height)),TRUE);};
        for(int i=0;i<4;++i)move(10+i,12+i*(w-24)/4,0,(w-32)/4,32);
        move(15,12,36,w-24,22);
        const int y=28;
        if(mode_==0) {
            move(100,12,44+y,72,24);move(30,90,40+y,w-236,240);move(50,w-134,40+y,122,30);
            move(101,12,82+y,56,24);move(31,70,78+y,half-58,240);move(102,half+24,82+y,60,24);move(32,half+88,78+y,half-64,240);
            move(40,12,118+y,156,26);move(41,176,118+y,178,26);move(42,362,118+y,180,26);move(33,w-148,114+y,136,160);
            move(103,12,150+y,w-24,22);move(34,12,176+y,w-24,64);
            const int buttons[]={51,52,53,54,55,56,58};for(int i=0;i<7;++i)move(buttons[i],12+i*(w-24)/7,250+y,(w-36)/7,32);
            move(90,12,292+y,w-24,38);move(35,12,330+y,w-24,220);move(80,12,370+y,w-24,std::max(80,h-410));
        } else {
            move(110,12,44+y,88,24);move(20,104,40+y,w-430,240);move(21,w-314,40+y,92,32);move(22,w-214,40+y,92,32);move(23,w-114,40+y,102,32);
            move(100,12,92+y,90,22);move(60,108,86+y,w-120,30);
            if(mode_==1) {
                const int x=236,r=w-x-12;
                move(24,12,78+y,210,std::max(120,h-150));
                move(110,x,44+y,72,24);move(20,x+76,40+y,r-76,240);
                move(21,x,82+y,(r-16)/3,32);move(22,x+(r-16)/3+8,82+y,(r-16)/3,32);move(23,x+((r-16)/3+8)*2,82+y,(r-16)/3,32);
                move(100,x,124+y,82,22);move(60,x+88,118+y,r-88,30);
                move(102,x,164+y,82,22);move(62,x+88,158+y,r-180,30);move(71,w-98,158+y,86,30);
                move(103,x,206+y,82,22);move(63,x+88,200+y,r-88,220);
                move(106,x,248+y,82,22);move(66,x+88,242+y,r-250,200);move(104,w-168,248+y,70,22);move(64,w-92,242+y,80,30);
                move(15,x,286+y,r,22);
                const int actions[]={72,74,73,57,77};for(int i=0;i<5;++i)move(actions[i],x+i*r/5,318+y,(r-16)/5,32);
                move(108,x,360+y,r,40);move(105,x,406+y,r,std::max(48,std::min(84,h-520)));
                move(53,x,h-78,120,32);move(76,x+132,h-78,r-132,32);
                move(59,x,h-38,r,28);
            } else if(mode_==2) {
                move(102,12,136+y,90,22);move(62,108,130+y,w-214,30);move(71,w-98,130+y,86,30);
                move(101,12,180+y,90,22);move(61,108,174+y,half-96,180);move(103,half+24,180+y,100,22);move(63,half+128,174+y,half-104,180);
                move(104,12,216+y,w-24,22);move(64,12,242+y,w-24,64);move(105,12,316+y,w-24,40);move(65,12,360+y,w-24,std::max(40,h-442));
            } else {move(104,12,132+y,w-24,24);move(64,12,164+y,w-24,std::max(80,h-246));}
            if(mode_!=1)move(59,12,h-76,w-24,28);move(90,12,h-32,w-24,26);
        }
    }
    /// Render hidden test pages using the same native controls and explicit edit text fallback.
    void print(HDC dc) {
        RECT area{};GetClientRect(window_,&area);FillRect(dc,&area,white_);
        for(HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            if(!(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE))continue;
            RECT r{};GetWindowRect(child,&r);POINT start{r.left,r.top};ScreenToClient(window_,&start);
            const int saved=SaveDC(dc);OffsetViewportOrgEx(dc,start.x,start.y,NULL);IntersectClipRect(dc,0,0,r.right-r.left,r.bottom-r.top);
            const int native=SaveDC(dc);SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dc,native);
            wchar_t kind[32]{};GetClassNameW(child,kind,32);
            if(wcscmp(kind,L"Button")==0 && (GetWindowLongW(child,GWL_STYLE)&BS_OWNERDRAW)) {
                wchar_t caption[256]{};GetWindowTextW(child,caption,256);RECT input{0,0,r.right-r.left,r.bottom-r.top};
                FillRect(dc,&input,white_);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(31,51,77));SelectObject(dc,font_);DrawTextW(dc,caption,-1,&input,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
            }
            const bool combo=wcscmp(kind,L"ComboBox")==0,edit=wcscmp(kind,L"Edit")==0;
            if(combo || edit) {
                wstring text;
                if(combo) {const LRESULT selected=SendMessageW(child,CB_GETCURSEL,0,0),count=SendMessageW(child,CB_GETLBTEXTLEN,selected,0);if(count>=0 && count<4096){text.resize(static_cast<size_t>(count)+1);SendMessageW(child,CB_GETLBTEXT,selected,reinterpret_cast<LPARAM>(&text[0]));text.resize(count);}}
                else {const int count=std::min(65536,GetWindowTextLengthW(child));text.resize(static_cast<size_t>(count)+1);GetWindowTextW(child,&text[0],count+1);text.resize(count);}
                RECT input{px(5),px(3),r.right-r.left-px(combo?24:18),r.bottom-r.top-px(3)};FillRect(dc,&input,white_);SelectObject(dc,font_);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(31,51,77));
                DrawTextW(dc,text.c_str(),-1,&input,DT_LEFT|DT_NOPREFIX|(combo?DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS:DT_WORDBREAK));
            }
            RestoreDC(dc,saved);
        }
    }
    /// Route local UI lifetime and timers while all processing belongs to the coordinator.
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<AiPage*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<AiPage*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,wparam,lparam);
        switch(message) {
        case WM_CREATE:self->font();self->white_=CreateSolidBrush(RGB(255,255,255));self->page_=CreateSolidBrush(RGB(246,248,252));self->build();SetTimer(window,1,250,NULL);return 0;
        case WM_SIZE:self->layout();return 0;
        case WM_COMMAND:{
            const UINT notify=HIWORD(wparam);
            if(notify==EN_SETFOCUS||notify==EN_KILLFOCUS||notify==EN_CHANGE||notify==EN_UPDATE||notify==EN_ERRSPACE||notify==EN_MAXTEXT||notify==EN_HSCROLL||notify==EN_VSCROLL)return 0;
            self->command(LOWORD(wparam),notify);return 0;
        }
        case WM_DRAWITEM:{
            const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);if(item->CtlType!=ODT_BUTTON) break;
            const bool tab=item->CtlID>=10 && item->CtlID<=13,primary=item->CtlID==22||item->CtlID==51||item->CtlID==72,selected=tab && item->CtlID-10==self->mode_,disabled=(item->itemState&ODS_DISABLED)!=0;
            const COLORREF fill=disabled?RGB(226,230,236):primary?RGB(43,105,230):selected?RGB(255,255,255):RGB(236,240,246);
            HBRUSH brush=CreateSolidBrush(fill);FillRect(item->hDC,&item->rcItem,brush);DeleteObject(brush);
            if(!primary){HBRUSH edge=CreateSolidBrush(selected?RGB(43,105,230):RGB(209,219,234));FrameRect(item->hDC,&item->rcItem,edge);DeleteObject(edge);}
            wchar_t caption[256]{};GetWindowTextW(item->hwndItem,caption,256);SetBkMode(item->hDC,TRANSPARENT);
            SetTextColor(item->hDC,disabled?RGB(143,155,171):primary?RGB(255,255,255):selected?RGB(43,105,230):RGB(51,65,85));
            HGDIOBJ old=SelectObject(item->hDC,self->font_);RECT text=item->rcItem;DrawTextW(item->hDC,caption,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);SelectObject(item->hDC,old);return TRUE;
        }
        case WM_TIMER:self->refresh();return 0;
        case WM_PRINT:self->print(reinterpret_cast<HDC>(wparam));return 0;
        case WM_ERASEBKGND:{RECT r{};GetClientRect(window,&r);FillRect(reinterpret_cast<HDC>(wparam),&r,self->page_);return 1;}
        case WM_CTLCOLORSTATIC:SetBkColor(reinterpret_cast<HDC>(wparam),RGB(246,248,252));SetTextColor(reinterpret_cast<HDC>(wparam),RGB(51,65,85));return reinterpret_cast<LRESULT>(self->page_);
        case WM_CTLCOLOREDIT:SetBkColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255));SetTextColor(reinterpret_cast<HDC>(wparam),RGB(31,51,77));return reinterpret_cast<LRESULT>(self->white_);
        case WM_NCDESTROY:KillTimer(window,1);instance()=NULL;SetWindowLongPtrW(window,GWLP_USERDATA,0);DeleteObject(self->font_);DeleteObject(self->white_);DeleteObject(self->page_);delete self;return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    /// GUI-thread-owned page handles, DPI, category and refresh markers.
    HWND window_=NULL;
    HFONT font_=NULL;
    HBRUSH white_=NULL,page_=NULL;
    int dpi_=96,mode_=0,locale_=-1;
    uint64_t revision_=static_cast<uint64_t>(-1);
    /// Stable combo identities never depend on translated captions.
    std::map<int,std::vector<string>> ids_;
    std::map<int,string> labels_;
    /// Current call, profile editor identity and exact displayed export bytes.
    wstring wav_;
    string editing_,shown_;
    bool unresolved_=false,enabledShown_=true;
    /// Installed-state/model observations and selected per-provider final-result files.
    uint64_t toolRevision_=static_cast<uint64_t>(-1);
    /// Install revision already followed by a model-list read, so the timer does not start another one.
    uint64_t modelsAfterInstall_=0;
    std::vector<wstring> resultFiles_;
};
#endif
