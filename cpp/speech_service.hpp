#pragma once

#ifdef _WIN64
#include <filesystem>

/// Stable speech observation copied under the coordinator mutex; no transcript in diagnostics.
struct SpeechState {
    /// Current stable preparation or execution code.
    string step = "off";
    /// Last preparation operation survives errors and cancellation for timeline rendering.
    string preparationStep;
    /// Decimal transfer counts, separated from localized labels.
    string completedBytes, totalBytes;
    /// Persisted, sanitized failure facts; separate from readiness and user preference changes.
    string failureCode, failedStep, resource, origin, detail, httpStatus, nativeCode, processExit, operation;
    /// Current percentage and waiting file count.
    int percent = 0, waiting = 0;
    /// User preference and one owned process operation.
    bool enabled = false, busy = false, installed = false, recording = false, liveEnabled = false, historyEnabled = false;
    /// Selected live engine and the short phase shown beside captions. Engine id stays ASCII.
    string liveEngine = "sense", livePhase;
    /// Model currently owning the shared worker or its last diagnostic.
    string operationModel;
    /// Last live recording identity is retained for viewing after hangup.
    wstring liveWav;
    /// Monotonic change marker for inexpensive native redraws.
    uint64_t revision = 0;
    /// Monotonic operation start for elapsed-time presentation while bytes are unavailable.
    ULONGLONG stepStarted = 0;
};

struct ImeLiveOwner;
/// Report whether this PC currently has a routed internet connection.
bool systemHasInternet();
/// Report whether the saved playback endpoint is still an active render device.
bool playbackDeviceActive(const wstring& deviceId);

/// Independent optional coordinator; receiving/closing a WAV never waits for recognition.
class SpeechService {
#ifdef PHONERECORD_SPEECH_TESTING
    /// Only the isolated native regression binary may inspect process ownership.
    friend struct SpeechFixture;
#endif
public:
    /// Release the complete process range and join before application shutdown.
    ~SpeechService();
    friend void speechLivePcm(const int16_t* pcm, int count);
    /// Read a bounded UTF-8 file, rejecting oversized metadata.
    static string read(const wstring& file, size_t limit = 8 * 1024 * 1024) {
        HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (input == INVALID_HANDLE_VALUE) return "";
        LARGE_INTEGER n{}; string result;
        if (GetFileSizeEx(input, &n) && n.QuadPart >= 0 && static_cast<uint64_t>(n.QuadPart) <= limit) {
            result.resize(static_cast<size_t>(n.QuadPart)); DWORD read = 0;
            if (!result.empty() && (!ReadFile(input, &result[0], static_cast<DWORD>(result.size()), &read, NULL) || read != result.size())) result.clear();
        }
        CloseHandle(input); return result;
    }
    /// Replace an owned metadata file atomically; errors leave the old file intact.
    static bool write(const wstring& file, const string& bytes) {
        const wstring temp = file + L".pending";
        { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out.write(bytes.data(), bytes.size()); out.flush(); if (!out) { DeleteFileW(temp.c_str()); return false; } }
        if (MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(temp.c_str()); return false;
    }
    /// Load independent speech preferences; disabled launches no thread, process or download.
    void start(const wstring& config) {
        config_ = config + L".speech"; queueFile_ = config + L".speech-queue";
        wchar_t local[32768]{}; if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local))) return;
        root_ = wstring(local) + L"\\PhoneRecord\\speech-v1";
#ifdef PHONERECORD_SPEECH_TESTING
        root_ = config + L".speech-test-runtime";
#endif
        const string saved = read(config_, 4096);
        language_ = jsonField(saved, "language"); if (!validLanguage(language_)) language_ = "auto";
        source_ = jsonField(saved, "source"); if (source_ != "hf-mirror.com") source_ = "huggingface.co";
        selectedModel_ = savedModel(jsonField(saved,"model"));
        state_.enabled = jsonField(saved, "enabled") == "1";
        const string liveField = jsonField(saved,"liveEnabled"), historyField = jsonField(saved,"historyEnabled");
        state_.liveEnabled = liveField.empty() ? state_.enabled : liveField == "1";
        state_.historyEnabled = !historyField.empty() ? historyField == "1" : state_.enabled && !state_.liveEnabled && !liveField.empty();
        state_.enabled = state_.historyEnabled || state_.liveEnabled;
        liveLanguage_ = jsonField(saved,"liveLanguage"); if (!validLanguage(liveLanguage_)) liveLanguage_ = language_;
        liveSource_ = jsonField(saved,"liveSource") == "hf-mirror.com" ? "hf-mirror.com" : "huggingface.co";
        liveEngine_ = jsonField(saved,"liveEngine") == "ime" ? "ime" : "sense";
        liveImeTip_ = jsonField(saved,"liveImeTip"); liveImeProfile_ = jsonField(saved,"liveImeProfile"); liveDevice_ = jsonField(saved,"liveDevice");
        state_.liveEngine = liveEngine_;
        state_.installed = installed(selectedModel_);
        state_.step = state_.enabled ? state_.installed ? "standby" : "unprepared" : "off";
        if (state_.enabled) {
            string failure = read(root_ + L"\\install-error.json", 16384);
            if (failure.empty()) failure = read(root_ + L"\\progress.json", 16384);
            if (isFailure(jsonField(failure,"step"))) { state_.operationModel=validModel(jsonField(failure,"model"))?jsonField(failure,"model"):selectedModel_; observeProgress(failure); }
            std::istringstream savedQueue(read(queueFile_, 1024 * 1024)); string row;
            while (queue_.size() < 1000 && std::getline(savedQueue, row)) {
                const wstring wav = utf8ToWide(jsonField(row, "wav")); const string language = jsonField(row, "language");
                if (validLanguage(language) && safeRecording(wav)) queue_.push_back({wav, language, savedModel(jsonField(row,"model"))});
            }
            launchThread();
        }
    }
    /// Copy a thread-safe snapshot without exposing raw file paths or worker errors.
    SpeechState state() const { std::lock_guard<std::mutex> lock(mutex_); auto s = state_; s.waiting = static_cast<int>(queue_.size()); return s; }
    /// Stable identities distinguish both Qwen sizes in settings, queues and results.
    static bool validModel(const string& model) { return model == "sense" || model == "qwen" || model == "qwen17"; }
    /// Preserve the legacy 0.6B identity and default old configuration files to SenseVoice.
    static string savedModel(const string& model) { return validModel(model) ? model : "sense"; }
    /// Each model has a distinct sidecar; legacy SenseVoice and 0.6B paths remain readable.
    static wstring resultSuffix(const string& model) { return model == "qwen17" ? L".qwen3asr-1.7b.transcript" : model == "qwen" ? L".qwen3asr.transcript" : L".transcript"; }
    /// Read the selected ASR language independently of UI locale.
    string language() const { std::lock_guard<std::mutex> lock(mutex_); return language_; }
    /// Observe a model-specific readiness marker without changing the live engine.
    bool readyFor(const string& model) const { std::lock_guard<std::mutex> lock(mutex_); return installed(model); }
    /// Read the explicit installation/history selection.
    string model() const { std::lock_guard<std::mutex> lock(mutex_); return selectedModel_; }
    /// Switch the explicit installation/history model; live transcription always uses SenseVoice.
    void selectModel(const string& model) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.busy || (!validModel(model))) return;
        selectedModel_ = model; state_.installed = installed(model); savePreferences();
        state_.step = state_.historyEnabled ? !state_.failureCode.empty() ? state_.failureCode : state_.installed ? "standby" : "unprepared" : state_.enabled ? state_.step : "off";
        ++state_.revision; wake_.notify_all();
    }
    /// Signal actual PCM arrival without running inference on the recorder thread.
    void recordingStarted(const wstring& wav);
    /// Drain the live worker only after the recorder closes and patches its WAV.
    void recordingEnded(const wstring& wav) {
        bool fallback = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.liveWav != wav && state_.liveEnabled) fallback = true;
            if (state_.liveWav == wav) {
                state_.recording = false;
                const bool imeOwned = imeActive_.load() && !imeFellBack_.load();
                if (!imeOwned) write(root_ + L"\\live.stop", "closed");
                fallback = !imeOwned && !liveCancelled_ && !livePending_ && (!liveRan_ || active_ != wav);
                ++state_.revision;
            }
        }
        if (fallback) enqueue(wav,true,"sense");
        finishIme(wav);
    }
    /// Read the explicit source; never automatically switch mirrors.
    string source() const { std::lock_guard<std::mutex> lock(mutex_); return source_; }
    /// Persist a validated preference patch; disabling cancels all admission and accepted jobs.
    bool configure(bool enabled, const string& language, const string& source, bool keepLive = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validLanguage(language) || (source != "huggingface.co" && source != "hf-mirror.com")) return false;
        const bool live = keepLive ? state_.liveEnabled : enabled;
        const bool previousHistory = state_.historyEnabled;
        state_.historyEnabled = enabled;
        if (!write(config_, preferences(enabled || live,language,source,live))) { state_.historyEnabled = previousHistory; return false; }
        state_.enabled = enabled || live; state_.liveEnabled = live; language_ = language; source_ = source;
        if (!enabled) { ++epoch_; install_ = false; livePending_ = false; queue_.clear(); persistQueue(false); state_.step = "off"; }
        else if (!state_.busy) state_.step = !state_.failureCode.empty() ? state_.failureCode : state_.installed ? "standby" : "unprepared";
        ++state_.revision;
        if (enabled) launchThread();
        wake_.notify_all(); return true;
    }
    /// Live preferences are independent of manual history transcription and never cancel Qwen jobs.
    bool configureLive(bool enabled, const string& language, const string& source) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validLanguage(language) || (source != "huggingface.co" && source != "hf-mirror.com")) return false;
        const string oldLanguage = liveLanguage_, oldSource = liveSource_;
        liveLanguage_ = language; liveSource_ = source;
        if (!write(config_,preferences(state_.historyEnabled || enabled,language_,source_,enabled))) { liveLanguage_=oldLanguage; liveSource_=oldSource; return false; }
        state_.liveEnabled = enabled; state_.enabled = state_.historyEnabled || enabled;
        if (!enabled) { livePending_=false; liveCancelled_=true; if (state_.recording && active_ == state_.liveWav && activeModel_ == "sense") ++epoch_; requestImeStop(); }
        if (enabled) launchThread(); ++state_.revision; wake_.notify_all(); return true;
    }
    /// History transcription is optional and never starts a download by itself.
    bool configureHistory(bool enabled, const string& language, const string& source) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validLanguage(language) || (source != "huggingface.co" && source != "hf-mirror.com")) return false;
        const bool previous = state_.historyEnabled;
        state_.historyEnabled = enabled;
        const bool master = enabled || state_.liveEnabled;
        if (!write(config_, preferences(master, language, source, state_.liveEnabled))) { state_.historyEnabled = previous; return false; }
        state_.enabled = master; language_ = language; source_ = source;
        if (!enabled) {
            if (install_ && installModel_ != "sense") install_ = false;
            const bool activeHistory = activeModel_ == "qwen" || activeModel_ == "qwen17";
            if (state_.busy && activeHistory) { ++epoch_; state_.step = "cancelled"; }
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Job& item) { return item.model != "sense"; }), queue_.end());
            persistQueue(!activeHistory);
            if (!state_.liveEnabled && !(state_.busy && activeHistory)) state_.step = "off";
        } else if (!state_.busy) state_.step = !state_.failureCode.empty() ? state_.failureCode : state_.installed ? "standby" : "unprepared";
        ++state_.revision;
        if (state_.enabled) launchThread();
        wake_.notify_all(); return true;
    }
    /// Read live-only language/source without affecting the Qwen defaults.
    string liveLanguage() const { std::lock_guard<std::mutex> lock(mutex_); return liveLanguage_; }
    /// Read the explicit live-caption engine. Missing preferences stay on SenseVoice.
    string liveEngine() const { std::lock_guard<std::mutex> lock(mutex_); return liveEngine_; }
    /// Persist the user's engine choice. Network availability never changes this selection.
    bool selectLiveEngine(const string& engine) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (engine != "ime" && engine != "sense") return false;
        const string previous = liveEngine_; liveEngine_ = engine; state_.liveEngine = engine;
        if (!state_.recording) state_.livePhase = engine == "ime" ? "ime_idle" : "sense";
        if (!write(config_, preferences(state_.enabled, language_, source_, state_.liveEnabled))) { liveEngine_ = previous; state_.liveEngine = previous; return false; }
        ++state_.revision; return true;
    }
    /// Remember which installed text service and playback endpoint the user picked.
    bool selectLiveIme(const string& tip, const string& profile, const string& device) {
        std::lock_guard<std::mutex> lock(mutex_);
        const string oldTip = liveImeTip_, oldProfile = liveImeProfile_, oldDevice = liveDevice_;
        liveImeTip_ = tip; liveImeProfile_ = profile; liveDevice_ = device;
        if (!write(config_, preferences(state_.enabled, language_, source_, state_.liveEnabled))) { liveImeTip_ = oldTip; liveImeProfile_ = oldProfile; liveDevice_ = oldDevice; return false; }
        ++state_.revision; return true;
    }
    /// Copy the saved input-method identity without enumerating text services.
    void liveImeSelection(string& tip, string& profile, string& device) const {
        std::lock_guard<std::mutex> lock(mutex_); tip = liveImeTip_; profile = liveImeProfile_; device = liveDevice_;
    }
    /// Read the explicit SenseVoice model source.
    string liveSource() const { std::lock_guard<std::mutex> lock(mutex_); return liveSource_; }
    /// Request explicit first-use setup; no installer is launched by ordinary transcription.
    void prepare(const string& model = "", const string& source = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.enabled || state_.busy || stopping_) return;
        installModel_ = model.empty() ? selectedModel_ : model;
        if (!validModel(installModel_)) return;
        if ((installModel_ == "qwen" || installModel_ == "qwen17") && !state_.historyEnabled) return;
        installSource_ = source.empty() ? source_ : source;
        if (installSource_ != "huggingface.co" && installSource_ != "hf-mirror.com") return;
        install_ = true; launchThread(); wake_.notify_all();
    }
    /// Admit a finalized file once, preserving stable language at admission and a bounded disk queue.
    bool enqueue(const wstring& wav, bool automatic = false, const string& requestedModel = "") {
        if (!safeRecording(wav)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const string model = requestedModel.empty() ? selectedModel_ : requestedModel;
        if (!validModel(model)) return false;
        if ((model == "qwen" || model == "qwen17") && !state_.historyEnabled) return false;
        if (!state_.enabled || stopping_ || queue_.size() >= 1000 || (active_ == wav && activeModel_ == model)) return false;
        for (const auto& item : queue_) if (item.wav == wav && item.model == model) return true;
        if (automatic && jsonField(read(wav + (resultSuffix(model) + L".json")), "status") == "complete") return true;
        queue_.push_back({wav, language_, model});
        if (!persistQueue()) { queue_.pop_back(); state_.step = "save_failed"; ++state_.revision; return false; }
        ++state_.revision; launchThread(); wake_.notify_all(); return true;
    }
    /// Query only the current module's admitted work for its local Stop action.
    bool pendingFor(const string& model) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (state_.busy && state_.operationModel==model) || (install_ && installModel_==model) || (model=="sense"&&livePending_) || std::any_of(queue_.begin(),queue_.end(),[&](const Job& item){return item.model==model;});
    }
    /// Stop one model without cancelling queued work from the other module.
    void cancelModel(const string& model) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!validModel(model))return;
        if(install_&&installModel_==model)install_=false;
        if(state_.busy&&state_.operationModel==model){++epoch_;preempt_=false;state_.step="cancelled";}
        if(model=="sense"){livePending_=false;liveCancelled_=true;}
        queue_.erase(std::remove_if(queue_.begin(),queue_.end(),[&](const Job& item){return item.model==model;}),queue_.end());
        persistQueue(activeModel_!=model);++state_.revision;wake_.notify_all();
    }
    /// Stop preparation and queued/running transcription without removing existing results or models.
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_); liveCancelled_ = true; ++epoch_; install_ = false; livePending_ = false; queue_.clear(); persistQueue(false);
        state_.step = "cancelled"; ++state_.revision; wake_.notify_all();
    }
    /// Cancel owned process work and preserve the active/remaining queue for the next enabled start.
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); if (stopping_) return; stopping_ = true; shutdownEpoch_ = epoch_; ++epoch_; wake_.notify_all(); }
        if (thread_.joinable()) thread_.join();
        discardImeOwner();
    }
    /// Resolve only ordinary existing WAV files within the configured recordings tree, excluding junctions.
    static bool safeRecording(const wstring& wav, bool growing = false) {
        try {
            namespace fs = std::filesystem;
            const fs::path root = fs::absolute(utf8ToWide(g_cfg.recordDir)).lexically_normal();
            const fs::path file = fs::absolute(wav).lexically_normal();
            const auto relative = file.lexically_relative(root);
            if (relative.empty() || relative.is_absolute() || relative.native().find(L"..") == 0 || file.extension() != L".wav") return false;
            fs::path part = root;
            for (const auto& component : relative) {
                part /= component; const DWORD flags = GetFileAttributesW(part.c_str());
                if (flags == INVALID_FILE_ATTRIBUTES || flags & FILE_ATTRIBUTE_REPARSE_POINT) return false;
            }
            if (!fs::is_regular_file(file)) return false;
            if (growing) return true;
            // Refuse a WAV still open for writing; reading never races the recorder's final header.
            HANDLE completed = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (completed == INVALID_HANDLE_VALUE) return false;
            CloseHandle(completed); return true;
        } catch (...) { return false; }
    }
private:
    /// Recognize terminal codes so missing/partial progress can never masquerade as success or setup readiness.
    static bool isFailure(const string& code) {
        static const std::set<string> codes = {"failed","disk","integrity","permission","runtime_failed","inference_failed","download_failed","wav","invalid_language","source_changed","unsupported_os","network_timeout","network_dns","network_proxy","network_tls","http_failed","launch_failed","process_failed","operation_timeout","in_use"};
        return codes.count(code) != 0;
    }
    /// Publish all fields from one bounded protocol snapshot, including the terminal frame after process exit.
    void observeProgress(const string& body, unsigned expectedEpoch = static_cast<unsigned>(-1)) {
        const string step = jsonField(body,"step"); if (step.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (expectedEpoch != static_cast<unsigned>(-1) && expectedEpoch != epoch_) return;
        if (body == lastProgress_) return; lastProgress_ = body;
        if (state_.step != step) state_.stepStarted = GetTickCount64();
        if(validModel(jsonField(body,"model")))state_.operationModel=jsonField(body,"model");
        state_.step = step; state_.percent = std::max(0,std::min(100,atoi(jsonField(body,"percent").c_str())));
        const string phase = jsonField(body,"phase");
        if (!phase.empty()) state_.preparationStep = phase;
        else if (step == "check" || step == "runtime" || step == "model" || step == "vad" || step == "verify" || step == "load") state_.preparationStep = step;
        state_.completedBytes = jsonField(body,"completed"); state_.totalBytes = jsonField(body,"total");
        state_.resource = jsonField(body,"resource"); state_.origin = jsonField(body,"origin"); state_.operation = jsonField(body,"operation");
        if (isFailure(step)) {
            state_.failureCode = step; state_.failedStep = state_.preparationStep; state_.percent = 0;
            state_.detail = jsonField(body,"detail"); state_.httpStatus = jsonField(body,"httpStatus");
            state_.nativeCode = jsonField(body,"nativeCode"); state_.processExit = jsonField(body,"exitCode");
        }
        ++state_.revision;
    }
    /// Record child-launch/exit failures even when no script can write a progress frame.
    void processFailure(const string& code, const string& detail, DWORD native = 0, unsigned expectedEpoch = static_cast<unsigned>(-1)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expectedEpoch != static_cast<unsigned>(-1) && expectedEpoch != epoch_) return;
        state_.step = state_.failureCode = code; state_.failedStep = state_.preparationStep; state_.detail = detail;
        state_.nativeCode = std::to_string(native); state_.percent = 0; ++state_.revision;
    }
    /// Persistent job identity; never carries a GUI object or live recording handle.
    struct Job { wstring wav; string language; string model = "sense"; bool live = false; };
    /// Each model owns a separate verified readiness marker.
    bool installed(const string& model) const { return GetFileAttributesW((root_ + (model == "qwen17" ? L"\\qwen\\installed-1.7b-v1" : model == "qwen" ? L"\\qwen\\installed-v1" : L"\\installed-v1")).c_str()) != INVALID_FILE_ATTRIBUTES; }
    /// Save model selection together with the existing independent speech preferences.
    bool savePreferences() { return write(config_,preferences(state_.enabled,language_,source_,state_.liveEnabled)); }
    /// Serialize both modules together so editing either preserves the other's defaults.
    string preferences(bool enabled,const string& language,const string& source,bool live) const {
        return "{\"enabled\":\"" + string(enabled ? "1" : "0") + "\",\"language\":" + jsonQuoted(language) + ",\"source\":" + jsonQuoted(source) + ",\"model\":" + jsonQuoted(selectedModel_) + ",\"liveEnabled\":\"" + string(live ? "1" : "0") + "\",\"historyEnabled\":\"" + string(state_.historyEnabled ? "1" : "0") + "\",\"liveLanguage\":" + jsonQuoted(liveLanguage_) + ",\"liveSource\":" + jsonQuoted(liveSource_) + ",\"liveEngine\":" + jsonQuoted(liveEngine_) + ",\"liveImeTip\":" + jsonQuoted(liveImeTip_) + ",\"liveImeProfile\":" + jsonQuoted(liveImeProfile_) + ",\"liveDevice\":" + jsonQuoted(liveDevice_) + "}";
    }
    /// Check the exact provider-supported language set.
    static bool validLanguage(const string& value) { return value == "auto" || value == "zh" || value == "en" || value == "yue" || value == "ja" || value == "ko"; }
    /// Serialize one argument using Windows CRT quoting, without a command shell.
    static wstring quote(const wstring& value) {
        wstring out = L"\""; size_t slash = 0;
        for (wchar_t ch : value) { if (ch == L'\\') { ++slash; continue; } out.append(ch == L'"' ? slash * 2 + 1 : slash, L'\\'); slash = 0; out += ch; }
        out.append(slash * 2, L'\\'); return out + L'"';
    }
    /// Start the coordinator once; caller holds its mutex.
    void launchThread() { if (!thread_.joinable() && !stopping_) thread_ = std::thread(&SpeechService::loop, this); }
    /// Save waiting jobs plus the active job, so an interrupted process can be retried after restart.
    bool persistQueue(bool includeActive = true) {
        string bytes;
        if (includeActive && !active_.empty()) bytes += "{\"wav\":" + jsonQuoted(wideToUtf8(active_)) + ",\"language\":" + jsonQuoted(activeLanguage_) + ",\"model\":" + jsonQuoted(activeModel_) + "}\n";
        for (const auto& item : queue_) bytes += "{\"wav\":" + jsonQuoted(wideToUtf8(item.wav)) + ",\"language\":" + jsonQuoted(item.language) + ",\"model\":" + jsonQuoted(item.model) + "}\n";
        return write(queueFile_, bytes);
    }
    /// Materialize trusted embedded scripts only on explicit installation/admitted inference.
    bool resource(int id, const wchar_t* name) {
        HRSRC entry = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA); if (!entry) return false;
        HGLOBAL data = LoadResource(NULL, entry); if (!data) return false;
        const char* bytes = static_cast<const char*>(LockResource(data));
        return bytes && write(root_ + L"\\" + name, string(bytes, SizeofResource(NULL, entry)));
    }
    /// Run one child in a kill-on-close Job Object; poll structured status and bound the operation.
    bool process(const wstring& exe, const wstring& args, unsigned epoch, ULONGLONG timeout) {
        if (epoch_ != epoch) return false;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{}; wstring command = quote(exe) + L" " + args;
        HANDLE job = CreateJobObjectW(NULL, NULL); if (!job) { processFailure("launch_failed","CreateJobObjectW failed",GetLastError()); return false; }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !CreateProcessW(exe.c_str(), &command[0], NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, root_.c_str(), &startup, &child)) { const DWORD error = GetLastError(); CloseHandle(job); processFailure("launch_failed","Creating the managed speech process failed",error); return false; }
        if (!AssignProcessToJobObject(job, child.hProcess)) { const DWORD error = GetLastError(); TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, INFINITE); CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(job); processFailure("launch_failed","AssignProcessToJobObject failed",error); return false; }
        ResumeThread(child.hThread); CloseHandle(child.hThread);
        const auto started = GetTickCount64(); bool done = false;
        while (epoch_ == epoch && GetTickCount64() - started < timeout) {
            if (epoch_ == epoch) observeProgress(read(root_ + L"\\progress.json",16384),epoch);
            if (WaitForSingleObject(child.hProcess, 100) == WAIT_OBJECT_0) { done = true; break; }
        }
        DWORD code = 1; if (done) GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(job); WaitForSingleObject(child.hProcess, INFINITE); CloseHandle(child.hProcess);
        if (epoch_ == epoch) {
            observeProgress(read(root_ + L"\\progress.json",16384),epoch);
            if (!done) processFailure("operation_timeout","The managed operation exceeded its deadline",0,epoch);
            else if (code != 0) {
                bool missing; { std::lock_guard<std::mutex> lock(mutex_); missing = state_.failureCode.empty(); }
                if (missing) processFailure("process_failed","The process exited without a failure report",code,epoch);
                std::lock_guard<std::mutex> lock(mutex_); state_.processExit = std::to_string(code);
            }
        }
        return done && code == 0 && epoch_ == epoch;
    }
    /// Execute one operation at a time; installed resources are never implicitly repaired/downloaded.
    void loop() {
        for (;;) {
            bool installing; Job task; string source, installModel; unsigned epoch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || (state_.enabled && (install_ || (livePending_ || std::any_of(queue_.begin(),queue_.end(),[&](const Job& j){return installed(j.model);})))); });
                if (stopping_) return;
                installing = install_; install_ = false; installingNow_ = installing; installModel = installModel_; source = installing ? installSource_ : source_; epoch = epoch_;
                if (!installing) {
                    if (livePending_) { task = {state_.liveWav,liveLanguage_,"sense",true}; livePending_ = false; liveRan_ = true; DeleteFileW((root_ + L"\\live.stop").c_str()); if (!state_.recording) write(root_ + L"\\live.stop","closed"); }
                    else { auto next = std::find_if(queue_.begin(),queue_.end(),[&](const Job& j){return installed(j.model);}); task = *next; queue_.erase(next); }
                    active_ = task.wav; activeLanguage_ = task.language; activeModel_ = task.model;
                }
                state_.operationModel = installing ? installModel : task.model;
                state_.busy = true; state_.step = installing ? "check" : "load"; state_.percent = 0;
                state_.failureCode.clear(); state_.failedStep.clear(); state_.detail.clear(); state_.httpStatus.clear(); state_.nativeCode.clear(); state_.processExit.clear();
                state_.resource.clear(); state_.origin.clear(); state_.operation.clear(); lastProgress_.clear();
                state_.stepStarted = GetTickCount64();
                state_.preparationStep = state_.step; state_.completedBytes.clear(); state_.totalBytes.clear(); ++state_.revision;
            }
            bool ok = false;
            HANDLE exclusive = CreateMutexW(NULL, FALSE, L"Local\\PhoneRecordSpeechRuntimeV1");
            const DWORD ownership = exclusive ? WaitForSingleObject(exclusive, 0) : WAIT_FAILED;
            const bool acquired = ownership == WAIT_OBJECT_0 || ownership == WAIT_ABANDONED;
            try {
                if (!acquired) throw std::runtime_error("speech runtime is already in use");
                std::filesystem::create_directories(root_);
                DeleteFileW((root_ + L"\\progress.json").c_str());
                if (resource(1902, L"worker.cjs") && resource(1903, L"NOTICE.md") && resource(1904,L"qwen-install.ps1") && resource(1905,L"qwen-worker.py") && resource(1906,L"qwen-assets.json")) {
                    if ((installing ? installModel : task.model) != "sense") {
                        resource(1901,L"install.ps1");
                        if (installing) {
                            wchar_t system[32768]{}; GetSystemDirectoryW(system,static_cast<UINT>(std::size(system)));
                            ok = process(wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe",L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quote(root_ + L"\\qwen-install.ps1") + L" -Root " + quote(root_) + L" -ProgressFile " + quote(root_ + L"\\progress.json") + L" -Source " + utf8ToWide(source) + L" -Variant " + (installModel == "qwen17" ? L"1.7B" : L"0.6B"),epoch,7200000);
                        } else if (safeRecording(task.wav)) {
                            const string body = "{\"variant\":" + jsonQuoted(task.model == "qwen17" ? "1.7B" : "0.6B") + ",\"root\":" + jsonQuoted(wideToUtf8(root_ + L"\\qwen")) + ",\"wav\":" + jsonQuoted(wideToUtf8(task.wav)) + ",\"language\":" + jsonQuoted(task.language) + ",\"live\":" + string(task.live ? "true" : "false") + ",\"stopFile\":" + jsonQuoted(wideToUtf8(root_ + L"\\live.stop")) + ",\"progress\":" + jsonQuoted(wideToUtf8(root_ + L"\\progress.json")) + "}";
                            if (write(root_ + L"\\job.json",body)) ok = process(root_ + L"\\qwen\\python\\python.exe",quote(root_ + L"\\qwen-worker.py") + L" " + quote(root_ + L"\\job.json"),epoch,12ULL*3600000);
                        }
                    } else
                    if (installing && resource(1901, L"install.ps1")) {
                        DeleteFileW((root_ + L"\\installed-v1").c_str());
                        wchar_t system[32768]{}; GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
                        const wstring exe = wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
                        ok = process(exe, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quote(root_ + L"\\install.ps1") + L" -Root " + quote(root_) + L" -ProgressFile " + quote(root_ + L"\\progress.json") + L" -Source " + utf8ToWide(source), epoch, 3600000);
                    } else if (!installing && safeRecording(task.wav,task.live)) {
                        const string body = "{\"root\":" + jsonQuoted(wideToUtf8(root_)) + ",\"wav\":" + jsonQuoted(wideToUtf8(task.wav)) + ",\"language\":" + jsonQuoted(task.language) + ",\"live\":" + string(task.live ? "true" : "false") + ",\"stopFile\":" + jsonQuoted(wideToUtf8(root_ + L"\\live.stop")) + ",\"progress\":" + jsonQuoted(wideToUtf8(root_ + L"\\progress.json")) + "}";
                        if (write(root_ + L"\\job.json", body)) ok = process(root_ + L"\\node-v24.11.0-win-x64\\node.exe", quote(root_ + L"\\worker.cjs") + L" " + quote(root_ + L"\\job.json"), epoch, 12ULL * 3600000);
                    }
                }
            } catch (const std::filesystem::filesystem_error& error) { processFailure("permission","Cannot prepare the private speech directory",static_cast<DWORD>(error.code().value())); ok = false; }
            catch (...) { ok = false; }
            if (acquired) ReleaseMutex(exclusive);
            if (exclusive) CloseHandle(exclusive);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!installing && !active_.empty()) {
                    if ((stopping_ && state_.enabled && shutdownEpoch_ == epoch) || preempt_) queue_.push_front({active_, activeLanguage_,activeModel_});
                    preempt_ = false;
                    active_.clear(); persistQueue();
                }
                state_.busy = false;
                installingNow_ = false; state_.installed = installed(selectedModel_);
                if (epoch_ == epoch) {
                    if (ok) { state_.step = installing ? "ready" : "complete"; state_.failureCode.clear(); }
                    else {
                        if (!acquired) state_.failureCode = "in_use";
                        if (state_.failureCode.empty()) state_.failureCode = "failed";
                        state_.step = state_.failureCode; state_.failedStep = state_.preparationStep; state_.percent = 0;
                        LOGW("SPEECH_PREPARATION_FAILED stage=%s code=%s http=%s native=%s exit=%s",state_.failedStep.c_str(),state_.failureCode.c_str(),state_.httpStatus.c_str(),state_.nativeCode.c_str(),state_.processExit.c_str());
                    }
                }
                ++state_.revision;
            }
        }
    }
    /// All mutable preferences, snapshots and queue records share one ownership mutex.
    mutable std::mutex mutex_;
    /// Wake only for explicit setup, file completion, preference changes or shutdown.
    std::condition_variable wake_;
    /// Lazily started coordinator thread; never created for a disabled installation.
    std::thread thread_;
    /// Job cancellation generation read without locking during process polling.
    std::atomic<unsigned> epoch_{0};
    /// Generation admitted before shutdown; explicitly cancelled work must not be resurrected.
    unsigned shutdownEpoch_ = 0;
    /// Stop/admission flags protected by mutex_.
    bool stopping_ = false, install_ = false, installingNow_ = false, livePending_ = false, liveRan_ = false, preempt_ = false, liveCancelled_ = false;
    /// Root, per-config preferences/queue, and currently owned WAV identity.
    wstring root_, config_, queueFile_, active_;
    /// Language captured for the running file and defaults for future admission.
    string installModel_ = "sense", installSource_ = "huggingface.co", liveLanguage_ = "auto", liveSource_ = "huggingface.co";
    /// History defaults and active job identity.
    string selectedModel_ = "sense", activeModel_ = "sense", activeLanguage_, language_ = "auto", source_ = "huggingface.co";
    /// Choose SenseVoice or the input-method player for one recording. Caller holds mutex_.
    void armLive(const wstring& wav, const string& engine, const string& tip, const string& profile, const string& device, bool network, bool deviceOk, std::shared_ptr<struct ImeLiveOwner>& starting);
    /// Play the hangup tail and join the input-method threads after the WAV is closed.
    void finishIme(const wstring& wav);
    /// Stop input-method playback without touching the recording file.
    void requestImeStop();
    /// Join and release the input-method owner. Safe during shutdown.
    void discardImeOwner();
    /// Copy PCM into the caption queue. Never waits on the renderer or the UI.
    void livePcm(const int16_t* pcm, int count);
    /// Switch this call to SenseVoice after the input method produces no new text.
    void fallbackFromIme(const char* phase);
    /// Detached public observation.
    SpeechState state_;
    /// Explicit live engine and the installed text service the user selected.
    string liveEngine_ = "sense", liveImeTip_, liveImeProfile_, liveDevice_;
    /// Input-method owner. Pointer operations use imeMutex_; the RTP path never takes mutex_.
    std::shared_ptr<struct ImeLiveOwner> imeOwner_;
    std::mutex imeMutex_;
    /// Cross-thread admission flags. The recording thread only loads them.
    std::atomic<bool> imeActive_{false}, imeFellBack_{false};
    /// Last consumed complete progress frame; fresh terminal facts invalidate the UI even at the same percentage.
    string lastProgress_;
    /// Bounded persistent waiting work.
    std::deque<Job> queue_;
};

#include "ime_caption.hpp"

inline SpeechService::~SpeechService() { stop(); }

/// One x64-only speech owner, independent of recorder and UI object lifetimes.
static SpeechService g_speech;
/// Admit successful recording finalization without executing inference on a recording thread.
static void speechRecordingCompleted(const string& path) {
    if (!g_speech.state().enabled) return;
    wchar_t absolute[32768]{};
    if (GetFullPathNameW(utf8ToWide(path).c_str(), static_cast<DWORD>(std::size(absolute)), absolute, NULL)) g_speech.recordingEnded(absolute);
}
/// Start observation only when actual audio has reached the WAV writer.
static void speechRecordingStarted(const string& path) {
    wchar_t absolute[32768]{};
    if (GetFullPathNameW(utf8ToWide(path).c_str(),static_cast<DWORD>(std::size(absolute)),absolute,NULL)) g_speech.recordingStarted(absolute);
}
/// Hand one decoded block to the caption queue. The recording thread does not play or wait.
static void speechLivePcm(const int16_t* pcm, int count) { if (pcm && count > 0) g_speech.livePcm(pcm, count); }
#else
static void speechRecordingStarted(const string&) {}
/// x86 builds have no speech owner, settings, resources, downloads or worker.
static void speechRecordingCompleted(const string&) {}
static void speechLivePcm(const int16_t*, int) {}
#endif
