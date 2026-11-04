#pragma once

/// Owned recording service, constructed before commands are accepted.
static std::unique_ptr<Recorder> g_recorder;

/// Perform local readiness checks without sending any HTTP request to the phone.
static bool runLocalChecks() {
    string failure;
    const auto fail = [&](const char* code) { if (failure.empty()) failure = code; };
    diagnosticUpdate([](DiagnosticSnapshot& state) { state.localHealth.status = 3; });
    const auto errors = g_cfg.errors();
    if (!errors.empty()) { diagnosticHealth(false, false, "CONFIG_INVALID"); LOGE("CONFIG_INVALID field=%s", errors.front().c_str()); return false; }
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = 0;
    InetPtonA(AF_INET, g_cfg.advertiseIp.c_str(), &address.sin_addr);
    if (probe == INVALID_SOCKET || bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = WSAGetLastError(); fail("LOCAL_IP_NOT_ASSIGNED");
        LOGE("LOCAL_IP_NOT_ASSIGNED ip=%s wsa=%d", g_cfg.advertiseIp.c_str(), error);
    } else LOGI("LOCAL_IP_OK address=%s", g_cfg.advertiseIp.c_str());
    if (probe != INVALID_SOCKET) closesocket(probe);
    if (!ensureDir(g_cfg.recordDir)) { fail("RECORD_DIR_NOT_WRITABLE"); LOGE("RECORD_DIR_CREATE_FAILED win32=%lu", GetLastError()); }
    wchar_t directory[MAX_PATH * 4]{};
    GetFullPathNameW(utf8ToWide(g_cfg.recordDir).c_str(), static_cast<DWORD>(std::size(directory)), directory, NULL);
    const wstring temp = wstring(directory) + L"\\.phonerecord-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) { fail("RECORD_DIR_NOT_WRITABLE"); LOGE("RECORD_DIR_NOT_WRITABLE win32=%lu", GetLastError()); }
    else { DWORD written = 0; const char data[] = "PhoneRecord local check"; if (!WriteFile(file, data, sizeof(data), &written, NULL) || written != sizeof(data)) fail("RECORD_DIR_NOT_WRITABLE"); CloseHandle(file); }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(directory, &freeBytes, NULL, NULL)) {
        LOGI("DISK_SPACE free_mib=%llu", freeBytes.QuadPart / (1024 * 1024));
        if (freeBytes.QuadPart < 100ULL * 1024 * 1024) fail("DISK_SPACE_LOW");
    }
    const auto state = diagnosticSnapshot();
    if (!state.controlListening) fail("CONTROL_BIND_FAILED");
    if (g_cfg.hookPort > 0 && !state.eventListening) fail("EVENT_BIND_FAILED");
    diagnosticHealth(false, failure.empty(), failure.empty() ? "LOCAL_CHECK_OK" : failure.c_str());
    LOGI("LOCAL_CHECK_COMPLETE passed=%d code=%s phone_http=false", failure.empty(), failure.empty() ? "LOCAL_CHECK_OK" : failure.c_str());
    return failure.empty();
}

/// Serialize the phone's scarce HTTP request budget and coalesce repeated call events.
class CommandCoordinator {
public:
    /// Join the sole operation worker on destruction.
    ~CommandCoordinator() { shutdown(); }
    /// Start the owned command loop after services have initialized.
    void start() { running_ = true; worker_ = std::thread(&CommandCoordinator::loop, this); }

    /// Queue a bounded operation; Stop cancels in-flight Start waits immediately.
    bool enqueue(const string& operation, const char* origin = "manual") {
        std::lock_guard<std::mutex> admission(g_updateAdmission);
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_ || g_stopping || g_updateInstalling) return false;
        if (operation == "start" && g_updateStartupPending) return false;
        if ((operation == "phone" || operation == "login" || operation == "compatibility" || operation == "start") && !g_cfg.errors().empty()) {
            if (operation == "compatibility") { CompatibilityReport report; report.verdict = "settings"; report.checkedAt = diagnosticTime(); report.items.push_back({"local", "fail", "local_fix"}); publishCompatibility(report); }
            LOGW("COMMAND_REQUIRES_VALID_CONFIG operation=%s", operation.c_str()); return false;
        }
        if (operation == "compatibility" && string(origin) == "startup") automaticCompatibilityPending_ = true;
        if (operation == "start") {
            if (callActive_ || g_sessionCount > 0) { LOGW("START_DUPLICATE_IGNORED"); return false; }
            g_lastCallActivity = GetTickCount64();
            callActive_ = true; g_callEnded = false; diagnosticBegin(origin);
            diagnosticUpdate([](DiagnosticSnapshot& state) { state.callActive = true; });
        } else if (operation == "stop") {
            g_lastCallActivity = GetTickCount64();
            callActive_ = false; g_callEnded = true;
            diagnosticUpdate([](DiagnosticSnapshot& state) { state.callActive = false; });
            queue_.erase(std::remove(queue_.begin(), queue_.end(), "start"), queue_.end());
        } else if ((operation == "phone" || operation == "login" || operation == "compatibility") && (callActive_ || g_sessionCount > 0)) {
            if (operation == "compatibility") { CompatibilityReport report; report.verdict = "deferred"; report.checkedAt = diagnosticTime(); publishCompatibility(report); }
            LOGW("PHONE_CHECK_DEFERRED call_active=true"); return false;
        }
        if (operation == "compatibility" && compatibilitySnapshot().verdict == "running") return false;
        if (std::find(queue_.begin(), queue_.end(), operation) != queue_.end()) return true;
        if (queue_.size() >= 8) { LOGW("COMMAND_QUEUE_FULL limit=8"); return false; }
        if (operation == "compatibility") beginCompatibility();
        if (operation == "stop" || operation == "start") queue_.push_front(operation); else queue_.push_back(operation);
        wake_.notify_one(); return true;
    }

    /// Reject new commands and optionally drain one explicit Stop before joining.
    void shutdown(bool stopPhone = false) {
        { std::lock_guard<std::mutex> guard(mutex_); running_ = false; queue_.clear(); if (stopPhone && !g_offline && diagnosticSnapshot().authenticated && (callActive_ || g_sessionCount > 0)) queue_.push_back("stop"); }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    /// Execute operations without retaining the queue mutex or blocking the GUI thread.
    void loop() {
        while (true) {
            string operation;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                wake_.wait_for(guard, std::chrono::milliseconds(500), [&]() { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) break;
                if (!queue_.empty()) { operation = queue_.front(); queue_.pop_front(); }
                else {
                    const int interval = g_healthIntervalSec.load();
                    const ULONGLONG now = GetTickCount64();
                    if (interval != scheduledInterval_) { scheduledInterval_ = interval; nextHealth_ = now + static_cast<ULONGLONG>(interval) * 1000; }
                    if (callActive_ || g_sessionCount > 0 || now - g_lastCallActivity.load() < 20000) continue;
                    if (automaticCompatibilityPending_) operation = "compatibility";
                    else {
                        if (interval <= 0 || now < nextHealth_) continue;
                        nextHealth_ = now + static_cast<ULONGLONG>(interval) * 1000;
                        operation = "health";
                    }
                }
            }
            {
                std::lock_guard<std::mutex> admission(g_updateAdmission);
                if (g_updateInstalling) continue;
                diagnosticUpdate([](DiagnosticSnapshot& state) { state.busy = true; });
            }
            LOGI("OPERATION_BEGIN operation=%s", operation.c_str());
            if (operation == "local") runLocalChecks();
            else if (operation == "login") g_phone.login();
            else if (operation == "compatibility") {
                runLocalChecks();
                if (!g_offline && !diagnosticSnapshot().authenticated && !diagnosticSnapshot().callActive && g_sessionCount == 0) g_phone.login();
                g_phone.checkCompatibility();
                // A startup inspection interrupted by a call gets one new opportunity
                // after the existing idle quiet period, independently of health polling.
                if (compatibilitySnapshot().verdict != "deferred") {
                    std::lock_guard<std::mutex> guard(mutex_);
                    automaticCompatibilityPending_ = false;
                }
            }
            else if (operation == "phone" || operation == "health") {
                if (operation == "health") runLocalChecks();
                // A call may have arrived after this check was queued. Never spend its
                // scarce phone HTTP budget on background diagnostics.
                const auto state = diagnosticSnapshot();
                if (state.callActive || g_sessionCount > 0) LOGI("PHONE_CHECK_DEFERRED call_active=true");
                else if (!g_offline) {
                    if (!state.authenticated && operation == "phone") g_phone.login();
                    if (diagnosticSnapshot().authenticated) g_phone.check();
                    else if (operation == "health") diagnosticHealth(true, false, "AUTH_REQUIRED");
                }
            } else if (operation == "start") {
                if (!diagnosticSnapshot().controlListening) diagnosticStage(RecordingStage::Failed, "CONTROL_BIND_FAILED");
                else {
                    if (!diagnosticSnapshot().authenticated && !g_callEnded) g_phone.login();
                    if (!g_callEnded && diagnosticSnapshot().authenticated) g_phone.startRecord();
                }
            } else if (operation == "stop") {
                if (!g_offline) g_phone.stopRecord();
                if (g_recorder) g_recorder->stopAll("USER_OR_EVENT_STOP");
                LOGI("STOP_COMPLETE session_finalize_requested=true");
            }
            LOGI("OPERATION_END operation=%s", operation.c_str());
            diagnosticUpdate([](DiagnosticSnapshot& state) { state.busy = false; });
        }
    }

    /// Serialize queue operations independently of the phone HTTP mutex.
    std::mutex mutex_;
    /// Wake a sleeping command worker without polling.
    std::condition_variable wake_;
    /// Owned command worker.
    std::thread worker_;
    /// At most eight coalesced operation names.
    std::deque<string> queue_;
    /// Worker lifetime, protected by mutex_.
    bool running_ = false;
    /// Deduplicate established-call events, protected by mutex_.
    bool callActive_ = false;
    /// Startup inspection awaiting an idle retry after a call; guarded by mutex_.
    bool automaticCompatibilityPending_ = false;
    /// Monotonic idle-check schedule, owned exclusively by the operation thread.
    ULONGLONG nextHealth_ = 0;
    int scheduledInterval_ = -1;
};

/// Single coordinator used by the GUI and acknowledged Action URL callbacks.
static CommandCoordinator g_commands;

/// Reserve a received established-call event until its response callback has been released.
struct PendingCallEvent {
    /// Construct while holding g_updateAdmission, before an installation can claim idle state.
    PendingCallEvent() { ++g_pendingCallEvents; }
    /// Release even when sending the HTTP acknowledgement fails and its callback is discarded.
    ~PendingCallEvent() { --g_pendingCallEvents; }
};

/// Accept existing phone event URLs while refusing unrelated paths and unknown event names.
static void hookHandler(HttpRequest& request, HttpResponse& response) {
    response.contentType = "text/plain; charset=utf-8";
    if (request.path == "/compatibility" && request.method == "GET" && request.sourceIp == "127.0.0.1") {
        response.contentType = "application/json"; response.body = compatibilityJson(); return;
    }
    if (request.path == "/health" && request.method == "GET" && request.sourceIp == "127.0.0.1") {
        const auto state = diagnosticSnapshot();
        response.contentType = "application/json";
        response.body = string("{\"version\":\"") + kProductVersion + "\",\"control\":" + (state.controlListening ? "true" : "false") + ",\"event\":" + (state.eventListening ? "true" : "false") + ",\"offline\":" + (g_offline ? "true" : "false")
            + ",\"call_active\":" + (state.callActive ? "true" : "false") + ",\"active_sessions\":" + std::to_string(g_sessionCount.load())
            + ",\"local_health\":" + std::to_string(state.localHealth.status) + ",\"phone_health\":" + std::to_string(state.phoneHealth.status)
            + ",\"local_code\":" + jsonQuoted(state.localHealth.code) + ",\"phone_code\":" + jsonQuoted(state.phoneHealth.code) + "}";
        return;
    }
    if (request.path != "/event" || request.method != "GET") { response.status = 404; response.body = "not found"; return; }
    if (request.sourceIp != g_cfg.phoneIp) { response.status = 403; response.body = "source rejected"; LOGW("EVENT_SOURCE_REJECTED peer=%s", request.sourceIp.c_str()); return; }
    const string event = toLower(request.query["ev"].empty() ? request.query["event"] : request.query["ev"]);
    const bool start = event == "est" || event == "established";
    const bool stop = event == "end" || event == "terminated";
    if (!start && !stop) { response.status = 400; response.body = "unknown event"; LOGW("EVENT_UNKNOWN_TYPE"); return; }
    std::shared_ptr<PendingCallEvent> admission;
    if (start) {
        std::lock_guard<std::mutex> guard(g_updateAdmission);
        if (g_updateInstalling || g_updateStartupPending) { response.status = 503; response.body = "update in progress"; return; }
        admission = std::make_shared<PendingCallEvent>();
    }
    diagnosticUpdate([](DiagnosticSnapshot& state) { ++state.events; });
    LOGI("EVENT_RECEIVED type=%s peer=%s", start ? "established" : "terminated", request.sourceIp.c_str());
    response.body = "OK";
    response.afterSend = [start, admission]() { g_commands.enqueue(start ? "start" : "stop", "phone_event"); };
}

/// Analyse an existing PCM WAV on explicit request, without altering its audio samples.
static bool analyseExistingWav(const string& path, const std::atomic<bool>* cancel = NULL) {
    std::ifstream input(utf8ToWide(path), std::ios::binary);
    char riff[12];
    if (!input.read(riff, 12) || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { LOGW("WAV_ANALYSE_UNSUPPORTED"); return false; }
    bool formatOk = false, found = false;
    PcmQuality quality;
    for (int chunks = 0; chunks < 256 && input; ++chunks) {
        char name[4]; uint32_t length = 0;
        if (!input.read(name, 4) || !input.read(reinterpret_cast<char*>(&length), 4)) break;
        if (!memcmp(name, "fmt ", 4)) {
            if (length < 16 || length > 65536) break;
            uint8_t format[16];
            if (!input.read(reinterpret_cast<char*>(format), 16)) break;
            const uint16_t type = format[0] | format[1] << 8, channels = format[2] | format[3] << 8, bits = format[14] | format[15] << 8;
            uint32_t rate = 0; memcpy(&rate, format + 4, 4);
            formatOk = type == 1 && channels == 1 && rate == 8000 && bits == 16;
            input.seekg(static_cast<std::streamoff>(length - 16 + (length & 1)), std::ios::cur);
        } else if (!memcmp(name, "data", 4)) {
            if (!formatOk || (length & 1)) break;
            uint32_t remaining = length;
            int16_t samples[4096];
            while (remaining && input && !g_stopping && (!cancel || !cancel->load())) {
                const uint32_t take = std::min<uint32_t>(remaining, sizeof(samples));
                if (!input.read(reinterpret_cast<char*>(samples), take)) break;
                quality.add(samples, take / 2); remaining -= take;
            }
            found = remaining == 0; break;
        } else input.seekg(static_cast<std::streamoff>(length) + (length & 1), std::ios::cur);
    }
    if (!found) { LOGW("WAV_ANALYSE_UNSUPPORTED_OR_TRUNCATED"); return false; }
    std::ofstream output(utf8ToWide(path + ".quality.json"), std::ios::binary | std::ios::trunc);
    output << "{\"quality\":\"" << quality.category() << "\",\"source\":\"historical_unverified\",\"duration\":" << quality.duration()
           << ",\"peak\":" << quality.peak() << ",\"rms\":" << quality.rms() << ",\"full_seconds\":" << quality.fullSeconds()
           << ",\"varying\":" << (quality.varying() ? "true" : "false") << "}\n";
    output.flush();
    LOGI("WAV_ANALYSED saved=%d quality=%s peak=%d rms=%.2f source=historical_unverified", static_cast<bool>(output), quality.category().c_str(), quality.peak(), quality.rms());
    return static_cast<bool>(output);
}

/// Atomically save a validated runtime copy, encrypting the stored password for this user.
static bool saveConfiguration(const Config& config) {
    string encrypted;
    if (!protectLocalSecret(config.phonePass, encrypted)) { LOGE("CONFIG_ENCRYPT_FAILED win32=%lu", GetLastError()); return false; }
    const wstring pending = g_configPath + L".pending";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    output << "# PhoneRecord 2 configuration - restart to apply\n"
           << "phone_ip=" << config.phoneIp << "\nphone_port=" << config.phonePort
           << "\nphone_user=" << config.phoneUser << "\nphone_pass_dpapi=" << encrypted
           << "\nadvertise_ip=" << config.advertiseIp << "\ncontrol_port=" << config.controlPort
           << "\nhook_port=" << config.hookPort << "\nui_port=" << config.uiPort
           << "\nudp_port_start=" << config.udpPortStart << "\nudp_port_end=" << config.udpPortEnd
           << "\nidle_timeout=" << config.idleTimeoutSec << "\nrecord_dir=" << config.recordDir
           << "\nhealth_interval=" << config.healthIntervalSec << "\nclose_action=" << config.closeAction
           << "\ntray_notifications=" << config.trayNotifications
           << "\nledger_file=" << config.ledgerFile << "\ndirectory_file=" << config.directoryFile << "\n";
    for (const auto& item : config.extra) output << item.first << '=' << item.second << "\n";
    output.flush();
    const bool written = static_cast<bool>(output); output.close();
    if (!written || !MoveFileExW(pending.c_str(), g_configPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { LOGE("CONFIG_SAVE_FAILED win32=%lu", GetLastError()); return false; }
    LOGI("CONFIG_SAVED password_protected=true restart_required=true");
    return true;
}

/// Start in the Windows UI language when no explicit product preference exists.
static string defaultInterfaceLanguage() {
    const LANGID language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(language) != LANG_CHINESE) return "en";
    const WORD region = SUBLANGID(language);
    return region == SUBLANG_CHINESE_TRADITIONAL || region == SUBLANG_CHINESE_HONGKONG || region == SUBLANG_CHINESE_MACAU ? "zh-TW" : "zh-CN";
}

/// Read the per-config language preference independently of connection settings and secrets.
static string loadInterfaceLanguage() {
    std::ifstream input(g_configPath + L".ui", std::ios::binary);
    char data[257]{}; input.read(data, 256);
    const string body(data, static_cast<size_t>(input.gcount()));
    for (const char* language : {"zh-CN", "zh-TW", "en"})
        if (body == string("language=") + language + "\n" || body == string("language=") + language + "\r\n") return language;
    const string fallback = defaultInterfaceLanguage();
    if (input.gcount()) LOGW("UI_LANGUAGE_UNSUPPORTED using=%s", fallback.c_str());
    return fallback;
}

/// Save only a language token, leaving even unsaved connection drafts and stored credentials intact.
static bool saveInterfaceLanguage(const string& language) {
    if (language != "zh-CN" && language != "zh-TW" && language != "en") return false;
    const wstring path = g_configPath + L".ui", pending = path + L".pending";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc); output << "language=" << language << '\n'; output.flush();
    const bool written = static_cast<bool>(output); output.close();
    if (!written || !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { LOGW("UI_PREFERENCE_SAVE_FAILED win32=%lu", GetLastError()); return false; }
    LOGI("UI_LANGUAGE_SAVED locale=%s connection_config_untouched=true", language.c_str()); return true;
}

/// Compose a self-contained, redacted operator report without exporting credentials or caller details.
static string diagnosticReport() {
    const auto state = diagnosticSnapshot();
    std::ostringstream output;
    output << "PhoneRecord " << kProductVersion << " diagnostic report\nGenerated: " << diagnosticTime()
           << "\nMode: " << (g_offline ? "OFFLINE / SYNTHETIC TEST" : "normal")
           << "\nPhone: " << g_cfg.phoneIp << ':' << g_cfg.phonePort << "\nAdvertise: " << g_cfg.advertiseIp
           << "\nControl port: " << g_cfg.controlPort << " listening=" << state.controlListening
           << "\nEvent port: " << g_cfg.hookPort << " listening=" << state.eventListening
           << "\nUDP range: " << g_cfg.udpPortStart << '-' << g_cfg.udpPortEnd
           << "\nHealth interval seconds: " << g_healthIntervalSec.load()
           << "\nLocal health: " << state.localHealth.status << " code=" << state.localHealth.code << " checked=" << state.localHealth.checkedAt
           << "\nPhone health: " << state.phoneHealth.status << " code=" << state.phoneHealth.code << " checked=" << state.phoneHealth.checkedAt
           << "\nCall active: " << state.callActive << " sessions=" << g_sessionCount.load()
           << "\nAuthentication verified: " << state.authenticated
           << "\nCode: " << state.code << "\nDetail: " << state.detail
           << "\nTrigger: " << state.triggerSource << " session=" << state.sessionTag
           << "\nCurrent attempt: http_sent=" << state.attemptHttp << " tcp_accepted=" << state.attemptTcp
           << " start_replied=" << state.attemptReply << " rtp_received=" << state.attemptRtp << " wav_opened=" << state.attemptWav
           << "\nAttempts=" << state.attempts << " events=" << state.events << " tcp=" << state.connections
           << " frames=" << state.frames << " replies=" << state.responses << " rtp=" << state.packets
           << " pcm_bytes=" << state.pcmBytes << " files=" << state.files
           << "\nQuality=" << state.quality << " peak=" << state.peak << " rms=" << state.rms
           << "\nLog storage: " << g_diagnosticLog.storageError()
           << "\nSignal checks do not establish real-phone provenance or two-way speech.\n\nRecent events (redacted):\n";
    const auto entries = g_diagnosticLog.entries();
    const size_t start = entries.size() > 600 ? entries.size() - 600 : 0;
    for (size_t index = start; index < entries.size(); ++index) {
        const auto& event = entries[index];
        output << event.time << " [" << event.level << "] [T" << event.thread << "] " << event.text << '\n';
    }
    return output.str();
}
