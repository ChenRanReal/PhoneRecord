#pragma once

/// Serialized Fanvil web client preserving the device's existing challenge protocol.
class PhoneClient {
public:
    /// Configure only the supported network-recording profile after an explicit plan confirmation.
    PhoneSetupResult configurePhone(bool apply, const string& expectedPlan, const string& restorePath = "");
    /// Authenticate once and verify a protected page; offline mode prohibits phone traffic.
    bool login() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (g_offline) { LOGI("AUTH_SKIPPED offline=true"); return false; }
        readyObserved_ = 0;
        if (g_cfg.secretError) return authFailure("CREDENTIAL_DECRYPT_FAILED");
        loadCookie();
        string head, body;
        if (!request("GET", "/", "", head, body)) return authFailure("PHONE_UNREACHABLE");
        if (head.find("503") != string::npos) return authFailure("PHONE_BUSY");
        string sessionCookie = headerValue(head, "Set-Cookie");
        const size_t auth = sessionCookie.find("auth=");
        if (auth == string::npos) sessionCookie.clear();
        else { sessionCookie = sessionCookie.substr(auth + 5); sessionCookie = sessionCookie.substr(0, sessionCookie.find(';')); }
        if (!sessionCookie.empty()) cookie_ = sessionCookie;
        if (!request("GET", "/key==nonce?now=" + std::to_string(GetTickCount64()), "", head, body)) return authFailure("AUTH_NONCE_FAILED");
        if (head.find("503") != string::npos) return authFailure("PHONE_BUSY");
        if (loginPage(body)) return authFailure("AUTH_NONCE_FAILED");
        const string nonceBody = trim(body);
        string nonce;
        const size_t newline = nonceBody.find_first_of("\r\n");
        for (char value : nonceBody.substr(0, newline)) if (isxdigit(static_cast<unsigned char>(value))) nonce += value;
        if (nonce.size() > 128) return authFailure("AUTH_NONCE_FAILED");
        LOGI("AUTH_CHALLENGE session_cookie_present=%d nonce_length=%zu", !sessionCookie.empty(), nonce.size());
        if (nonce.size() < 8) {
            // A successful request must enter the cache-validation branch. The previous
            // implementation tested !request(), then cleared an otherwise valid cookie.
            if (!cookie_.empty() && request("GET", "/record.htm", "", head, body) && protectedPage(head, body)) {
                rememberRecordState(body); saveCookie(); return authSuccess("AUTH_CACHE_VALID");
            }
            return authFailure("AUTH_SESSION_INVALID");
        }
        cookie_ = nonce;
        const string digest = Md5::hex(g_cfg.phoneUser + ":" + g_cfg.phonePass + ":" + nonce);
        const string form = "encoded=" + urlEncode(g_cfg.phoneUser + ":" + digest) + "&CurLanguage=cn&ReturnPage=/";
        if (!request("POST", "/", form, head, body)) return authFailure("AUTH_POST_FAILED");
        if (head.find("503") != string::npos) return authFailure("PHONE_BUSY");
        if (loginPage(body) || head.find("200") == string::npos) return authFailure("AUTH_REJECTED");
        if (!sessionCookie.empty()) cookie_ = sessionCookie;
        if (!request("GET", "/record.htm", "", head, body) || !protectedPage(head, body)) return authFailure("AUTH_VERIFY_FAILED");
        rememberRecordState(body);
        saveCookie();
        return authSuccess("AUTH_VERIFIED");
    }

    /// Send a single Start action and wait only for local RTP evidence, never extra HTTP polling.
    bool startRecord() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (g_offline) return operationFailure("OFFLINE_MODE");
        string head, body;
        // Reuse a recent idle observation from login/health; querying again here costs
        // another 1.1-second device quiet interval before Start can be sent.
        const bool recentReady = readyObserved_ && GetTickCount64() - readyObserved_ <= 65000;
        const int state = recentReady ? 4 : recordState();
        LOGI("START_GUARD record_status=%d recent_ready=%d", state, recentReady);
        if (state < 0) return operationFailure(stale_ ? "AUTH_EXPIRED" : "PHONE_STATE_UNAVAILABLE");
        if (state != 4) {
            LOGW("START_STALE_STATE record_status=%d reset_once=true", state);
            if (!action("Stop", head, body)) return operationFailure("STALE_STATE_RESET_FAILED");
            Sleep(1500);
            if (recordState() != 4) return operationFailure("STALE_STATE_RESET_FAILED");
        }
        const uint64_t before = diagnosticSnapshot().packets;
        if (g_callEnded) return operationFailure("CALL_ENDED_BEFORE_START");
        diagnosticStage(RecordingStage::Command, "START_HTTP_SENT");
        if (!action("Start", head, body)) return false;
        LOGI("START_ACCEPTED wait_rtp_ms=8000 repeat_start=false");
        for (int elapsed = 0; elapsed < 8000 && !g_stopping && !g_callEnded; elapsed += 100) {
            if (diagnosticSnapshot().packets > before) { LOGI("START_MEDIA_CONFIRMED elapsed_ms=%d", elapsed); return true; }
            Sleep(100);
        }
        if (g_callEnded) return operationFailure("CALL_ENDED_BEFORE_MEDIA");
        const auto current = diagnosticSnapshot();
        if (current.attemptWav && current.packets > before) return true;
        if (current.attemptReply) return operationFailure("RTP_NOT_RECEIVED");
        if (current.attemptTcp) return operationFailure("CONTROL_NO_FRAME");
        return operationFailure("CONTROL_NOT_OBSERVED");
    }

    /// Send the Stop CGI once; an HTTP acknowledgement is not a state-machine proof.
    bool stopRecord() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (g_offline) return operationFailure("OFFLINE_MODE");
        string head, body;
        const bool ok = action("Stop", head, body);
        if (ok) LOGI("STOP_HTTP_ACCEPTED");
        return ok;
    }

    /// Check protected-page health independently of the last call's recording outcome.
    bool check() {
        std::lock_guard<std::mutex> guard(mutex_);
        const HealthResult previous = diagnosticSnapshot().phoneHealth;
        diagnosticUpdate([](DiagnosticSnapshot& value) { value.phoneHealth.status = 3; });
        const int state = recordState(true);
        if (state == -2) { diagnosticUpdate([&](DiagnosticSnapshot& value) { value.phoneHealth = previous; }); LOGI("PHONE_CHECK_DEFERRED call_arrived=true"); return false; }
        if (state < 0) {
            const char* code = stale_ ? "AUTH_EXPIRED" : "PHONE_STATE_UNAVAILABLE";
            diagnosticHealth(true, false, code); LOGW("%s check_only=true", code); return false;
        }
        LOGI("PHONE_CHECK record_status=%d control_port=%d event_port=%d", state, g_cfg.controlPort, g_cfg.hookPort);
        diagnosticHealth(true, true, "PHONE_CHECK_OK");
        return true;
    }

    /// Inspect the saved runtime profile using GET only; a real call remains the audio acceptance test.
    void checkCompatibility() {
        std::lock_guard<std::mutex> guard(mutex_);
        CompatibilityReport report = compatibilitySnapshot();
        report.items.clear(); report.checkedAt = diagnosticTime(); report.baselineAttempts = diagnosticSnapshot().attempts;
        report.externalTrigger = g_cfg.hookPort == 0;
        const auto add = [&](const char* key, const char* status, const char* advice) { report.items.push_back({key, status, advice}); };
        const auto done = [&](const char* verdict) { report.verdict = verdict; LOGI("COMPATIBILITY_RESULT verdict=%s checks=%zu readonly=true addresses_hidden=true", verdict, report.items.size()); publishCompatibility(report); };
        if (g_offline) { add("authentication", "skip", "offline"); done("offline"); return; }
        if (diagnosticSnapshot().callActive || g_sessionCount > 0) { add("idle", "pending", "idle"); done("deferred"); return; }
        const bool local = diagnosticSnapshot().localHealth.status == 1;
        add("local", local ? "pass" : "fail", local ? "local_ok" : "local_fix");
        if (!diagnosticSnapshot().authenticated) { add("authentication", "pending", "auth_fix"); done("unavailable"); return; }
        string head, body;
        if (!request("GET", "/record.htm", "", head, body, true) || !protectedPage(head, body)) {
            if (loginPage(body) || head.find("401") != string::npos) { stale_ = true; diagnosticUpdate([](DiagnosticSnapshot& state) { state.authenticated = false; }); diagnosticHealth(true, false, "AUTH_EXPIRED"); }
            add("authentication", "pending", "auth_fix"); done(requestDeferred_ ? "deferred" : "unavailable"); return;
        }
        add("authentication", "pass", "auth_ok");
        const auto fields = readPhoneForm(body);
        const char* required[] = {"EnableRecord_RW", "CSE_RecordType_RW", "CSE_RecordVoiceCodec_R", "SYS_RecordAddr_RW", "SYS_RecordPort_RW"};
        for (const char* key : required) if (fields.find(key) == fields.end()) { add("profile", "pending", "profile_missing"); done("unsupported"); return; }
        report.recognizedProfile = true; add("profile", "pass", "profile_ok");
        const auto value = [&](const char* key) { return fields.at(key); };
        const bool enabled = value("EnableRecord_RW") == "1";
        const bool network = value("CSE_RecordType_RW") == "1";
        const string codec = toLower(value("CSE_RecordVoiceCodec_R"));
        const bool supportedCodec = codec == "pcma" || codec == "pcmu";
        const bool address = value("SYS_RecordAddr_RW") == g_cfg.advertiseIp;
        const bool port = configurationInteger(value("SYS_RecordPort_RW")) == g_cfg.controlPort;
        add("enabled", enabled ? "pass" : "fail", enabled ? "enabled_ok" : "enabled_fix");
        add("mode", network ? "pass" : "fail", network ? "mode_ok" : "mode_fix");
        add("codec", supportedCodec ? "pass" : "fail", supportedCodec ? "codec_ok" : "codec_fix");
        add("address", address ? "pass" : "fail", address ? "address_ok" : "address_fix");
        add("control_port", port ? "pass" : "fail", port ? "port_ok" : "port_fix");
        bool callbacks = true;
        if (report.externalTrigger) add("callbacks", "skip", "external");
        else {
            if (!request("GET", "/action.htm", "", head, body, true) || head.find("200") == string::npos || loginPage(body)) {
                if (loginPage(body) || head.find("401") != string::npos) { stale_ = true; diagnosticUpdate([](DiagnosticSnapshot& state) { state.authenticated = false; }); diagnosticHealth(true, false, "AUTH_EXPIRED"); }
                add("callbacks", "pending", "callbacks_read"); done(requestDeferred_ ? "deferred" : "unavailable"); return;
            }
            const auto actions = readPhoneForm(body);
            const auto established = actions.find("AURL_CallEstablished_RW"), ended = actions.find("AURL_CallTerminated_RW");
            if (established == actions.end() || ended == actions.end()) { add("callbacks", "pending", "callbacks_missing"); done("unsupported"); return; }
            const bool est = compatibleCallback(established->second, "est"), end = compatibleCallback(ended->second, "end");
            callbacks = est && end;
            add("established", est ? "pass" : "fail", est ? "callback_ok" : "callback_fix");
            add("terminated", end ? "pass" : "fail", end ? "callback_ok" : "callback_fix");
        }
        add("runtime", "pending", "call_test"); add("speech", "pending", "speech");
        done(local && enabled && network && supportedCodec && address && port && callbacks ? "ready" : "settings");
    }

    /// Report whether a previous action observed a login page or authentication rejection.
    bool staleSession() const { return stale_; }

private:
    /// Mark authentication failure without revealing credentials or invalidating a good cache.
    bool authFailure(const char* code) {
        readyObserved_ = 0;
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.authenticated = false; });
        diagnosticHealth(true, false, code);
        diagnosticStage(RecordingStage::Failed, code);
        LOGE("%s", code); return false;
    }

    /// Mark protected-page authentication success independently of listener readiness.
    bool authSuccess(const char* code) {
        stale_ = false;
        diagnosticUpdate([&](DiagnosticSnapshot& state) { state.authenticated = true; if (!state.callActive) { state.code = code; state.stage = RecordingStage::Waiting; } });
        diagnosticHealth(true, true, code);
        LOGI("%s", code); return true;
    }

    /// Expose a stable action failure code and return false for worker orchestration.
    bool operationFailure(const char* code) {
        diagnosticStage(RecordingStage::Failed, code); LOGW("%s", code); return false;
    }

    /// Identify the device's HTTP-200 login-page disguise.
    static bool loginPage(const string& body) {
        return body.find("logon_content") != string::npos || body.find("Password Error") != string::npos || body.find("Not Authorized") != string::npos;
    }

    /// Require an authenticated recording page, not merely a successful HTTP status.
    static bool protectedPage(const string& head, const string& body) {
        return head.find("200") != string::npos && !loginPage(body) && body.find("recordStatus") != string::npos;
    }

    /// Parse recording state using the device page already cleaned by the HTTP transport.
    int recordState(bool idleOnly = false) {
        readyObserved_ = 0;
        string head, body;
        if (!request("GET", "/record.htm", "", head, body, idleOnly) || !protectedPage(head, body)) {
            if (requestDeferred_) return -2;
            if (loginPage(body)) { stale_ = true; diagnosticUpdate([](DiagnosticSnapshot& state) { state.authenticated = false; }); }
            return -1;
        }
        return rememberRecordState(body);
    }

    /// Retain only a protected-page idle observation, bounded to one health-check cycle.
    int rememberRecordState(const string& body) {
        std::smatch match;
        const int state = std::regex_search(body, match, std::regex("recordStatus\\s*=\\s*\"\\s*(\\d+)")) ? atoi(match[1].str().c_str()) : -1;
        readyObserved_ = state == 4 ? GetTickCount64() : 0;
        return state;
    }

    /// Post one CGI action; only explicit HTTP 503 permits a bounded retry.
    bool action(const char* name, string& head, string& body) {
        // An accepted Stop is not proof of idle state, and a failed Start may have
        // reached the phone. Either action consumes the previous ready observation.
        readyObserved_ = 0;
        stale_ = false;
        for (int attempt = 0; attempt < 3 && !g_stopping; ++attempt) {
            if (!request("POST", string("/") + name, string("DefaultRecord=") + name, head, body)) return operationFailure("HTTP_TRANSPORT_FAILED");
            if (head.find("503") != string::npos) {
                LOGW("HTTP_BUSY action=%s attempt=%d backoff_ms=%d", name, attempt + 1, 1500 * (attempt + 1));
                if (attempt < 2) { Sleep(1500 * (attempt + 1)); continue; }
                return operationFailure("PHONE_BUSY");
            }
            if (head.find("401") != string::npos || loginPage(body)) {
                stale_ = true;
                diagnosticUpdate([](DiagnosticSnapshot& state) { state.authenticated = false; });
                return operationFailure("AUTH_EXPIRED");
            }
            if (head.find("200") == string::npos) return operationFailure("HTTP_ACTION_REJECTED");
            return true;
        }
        return false;
    }

    /// Read a credential cache, accepting the established plaintext format for migration.
    void loadCookie() {
        std::ifstream input(".phone_cookie", std::ios::binary);
        char buffer[8193]; input.read(buffer, sizeof(buffer));
        const size_t length = static_cast<size_t>(input.gcount());
        if (length > 8192) { LOGW("COOKIE_CACHE_TOO_LARGE"); return; }
        const string value(buffer, length);
        string plain;
        if (unprotectLocalSecret(trim(value), plain) && plain.size() >= 8 && plain.size() <= 128) cookie_ = plain;
    }

    /// Persist a validated cookie using account-bound Windows encryption.
    void saveCookie() {
        string encoded;
        if (!protectLocalSecret(cookie_, encoded)) { LOGW("COOKIE_SAVE_FAILED encrypt=true"); return; }
        std::ofstream output(".phone_cookie", std::ios::binary | std::ios::trunc);
        output << encoded;
        if (!output) LOGW("COOKIE_SAVE_FAILED storage=true");
    }

    /// Encode form fields without changing the existing Fanvil challenge inputs.
    static string urlEncode(const string& text) {
        string result;
        for (unsigned char value : text) {
            if (isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~') result += static_cast<char>(value);
            else if (value == ' ') result += '+';
            else { char hex[4]; snprintf(hex, sizeof(hex), "%%%02X", value); result += hex; }
        }
        return result;
    }

    /// Extract a case-insensitive header value without retaining the complete response in logs.
    static string headerValue(const string& head, const string& name) {
        const string needle = toLower(name) + ":";
        size_t position = toLower(head).find(needle);
        if (position == string::npos) return "";
        position += needle.size();
        return trim(head.substr(position, head.find("\r\n", position) - position));
    }

    /// Reuse the known device-specific dechunking contract for legacy malformed responses.
    static string dechunk(const string& raw) {
        if (raw.find("\r\n") == string::npos) {
            const size_t newline = raw.find('\n');
            if (newline == string::npos) return raw;
            const string first = trim(raw.substr(0, newline));
            if (!first.empty() && first.size() <= 8 && first.find_first_not_of("0123456789abcdefABCDEF") == string::npos) return trim(raw.substr(newline + 1));
            return raw;
        }
        string result;
        size_t position = 0;
        while (position < raw.size()) {
            const size_t end = raw.find("\r\n", position);
            if (end == string::npos) { result += raw.substr(position); break; }
            string length = trim(raw.substr(position, end - position));
            length = length.substr(0, length.find(';'));
            if (length.empty() || length.size() > 8 || length.find_first_not_of("0123456789abcdefABCDEF") != string::npos) { result += raw.substr(position); break; }
            const unsigned long bytes = strtoul(length.c_str(), NULL, 16);
            if (!bytes) break;
            const size_t start = end + 2;
            result += raw.substr(start, std::min<size_t>(bytes, raw.size() - start));
            if (bytes > raw.size() - start) break;
            position = start + bytes;
            if (raw.compare(position, 2, "\r\n") == 0) position += 2;
        }
        return result.empty() ? raw : result;
    }

    /// Perform one paced, bounded IPv4 HTTP exchange; record metadata only.
    bool request(const string& method, const string& path, const string& body, string& head, string& content, bool idleOnly = false) {
        head.clear(); content.clear();
        requestDeferred_ = false;
        if (g_offline || g_stopping) return false;
        if (lastCompleted_ != std::chrono::steady_clock::time_point{})
            std::this_thread::sleep_until(lastCompleted_ + std::chrono::milliseconds(1100));
        if (idleOnly && (diagnosticSnapshot().callActive || g_sessionCount > 0)) { requestDeferred_ = true; return false; }
        /// Retain pacing on every return path, including partial/failed exchanges.
        struct CompleteExchange {
            /// Owning client's last completed HTTP exchange timestamp.
            std::chrono::steady_clock::time_point& completed;
            /// Start the next device quiet interval when this exchange finishes.
            ~CompleteExchange() { completed = std::chrono::steady_clock::now(); }
        } completion{lastCompleted_};
        const ULONGLONG started = GetTickCount64();
        SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection == INVALID_SOCKET) return false;
        u_long nonblocking = 1; ioctlsocket(connection, FIONBIO, &nonblocking);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<u_short>(g_cfg.phonePort));
        InetPtonA(AF_INET, g_cfg.phoneIp.c_str(), &address.sin_addr);
        int connected = connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (connected != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, errors; FD_ZERO(&writable); FD_ZERO(&errors); FD_SET(connection, &writable); FD_SET(connection, &errors);
            timeval wait{3, 0};
            connected = select(0, NULL, &writable, &errors, &wait);
            int error = 0, length = sizeof(error);
            if (connected > 0) getsockopt(connection, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
            connected = connected > 0 && !error ? 0 : SOCKET_ERROR;
        }
        if (connected != 0) { LOGE("HTTP_CONNECT_FAILED target=%s:%d", g_cfg.phoneIp.c_str(), g_cfg.phonePort); closesocket(connection); return false; }
        if (idleOnly && (diagnosticSnapshot().callActive || g_sessionCount > 0)) { requestDeferred_ = true; closesocket(connection); return false; }
        std::ostringstream request;
        request << method << ' ' << path << " HTTP/1.1\r\nHost: " << g_cfg.phoneIp << "\r\nUser-Agent: Mozilla/5.0\r\n";
        if (!body.empty()) request << "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " << body.size() << "\r\n";
        request << "Referer: http://" << g_cfg.phoneIp << path << "\r\n";
        if (!cookie_.empty()) request << "Cookie: auth=" << cookie_ << "\r\n";
        request << "Connection: close\r\n\r\n" << body;
        const string payload = request.str();
        const string safePath = path.substr(0, path.find('?'));
        LOGI("HTTP_SEND method=%s path=%s bytes=%zu cookie_present=%d", method.c_str(), safePath.c_str(), payload.size(), !cookie_.empty());
        if (!sendComplete(connection, payload, 3000)) { LOGE("HTTP_SEND_FAILED wsa=%d", WSAGetLastError()); closesocket(connection); return false; }
        if (path == "/Start") diagnosticUpdate([](DiagnosticSnapshot& state) { state.attemptHttp = true; });
        string response;
        const ULONGLONG deadline = GetTickCount64() + 6000;
        bool complete = false;
        while (GetTickCount64() < deadline && !g_stopping) {
            if (idleOnly && (diagnosticSnapshot().callActive || g_sessionCount > 0)) { requestDeferred_ = true; break; }
            fd_set readable; FD_ZERO(&readable); FD_SET(connection, &readable);
            timeval wait{0, 200000};
            const int ready = select(0, &readable, NULL, NULL, &wait);
            if (ready == SOCKET_ERROR) break;
            if (!ready) continue;
            char buffer[8192];
            const int received = recv(connection, buffer, sizeof(buffer), 0);
            if (!received) { complete = true; break; }
            if (received == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) continue; break; }
            response.append(buffer, received);
            if (response.size() > 4 * 1024 * 1024) { LOGE("HTTP_RESPONSE_LIMIT"); break; }
        }
        closesocket(connection);
        if (requestDeferred_) return false;
        const size_t split = response.find("\r\n\r\n");
        if (split == string::npos || !complete) { LOGE("HTTP_RESPONSE_INCOMPLETE bytes=%zu elapsed_ms=%llu", response.size(), GetTickCount64() - started); return false; }
        head = response.substr(0, split); content = response.substr(split + 4);
        if (toLower(head).find("chunked") != string::npos) content = dechunk(content);
        LOGI("HTTP_RESULT method=%s path=%s status=%s body_bytes=%zu login_page=%d elapsed_ms=%llu", method.c_str(), safePath.c_str(), head.substr(0, head.find("\r\n")).c_str(), content.size(), loginPage(content), GetTickCount64() - started);
        return true;
    }

    /// Current authentication cookie; never included in diagnostic exports.
    string cookie_;
    /// Most recent action observed authentication expiry.
    bool stale_ = false;
    /// Monotonic time of the last protected-page idle state, invalidated by all actions.
    ULONGLONG readyObserved_ = 0;
    /// Idle checks yield to a newly established call instead of delaying its recording command.
    bool requestDeferred_ = false;
    /// High-resolution completion time; each following exchange waits at least 1.1 seconds.
    std::chrono::steady_clock::time_point lastCompleted_{};
    /// Serialize authentication, checks and Start/Stop without locking the GUI thread.
    std::mutex mutex_;
};

#include "phone_setup_client.hpp"
