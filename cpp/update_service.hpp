#pragma once

#include <winhttp.h>
#include "json_text.hpp"
#include "update_protocol.hpp"

#pragma comment(lib, "winhttp.lib")

#ifdef PHONERECORD_UPDATE_TESTING
/// Local fixtures exist only in the separate test binary; the shipped exe has no test URL/key switches.
static std::function<DWORD(const wstring&, string&, size_t, const wstring&, const std::function<void(size_t)>&)> g_updateTestHttp;
#endif

/// Cancelable, bounded HTTPS downloads; never shares the phone HTTP connection or credentials.
class UpdateHttp {
public:
    /// Abort the current request before its owner joins the network worker.
    void cancel() { std::lock_guard<std::mutex> guard(mutex_); stopped_ = true; if (request_) { WinHttpCloseHandle(request_); request_ = NULL; } }

    /// Fetch a fixed GitHub HTTPS URL into bounded memory or an owned staging file.
    DWORD get(const wstring& url, string& bytes, size_t limit, const wstring& destination = L"", const std::function<void(size_t)>& progress = {}) {
#ifdef PHONERECORD_UPDATE_TESTING
        if (g_updateTestHttp) return g_updateTestHttp(url, bytes, limit, destination, progress);
#endif
        bytes.clear();
        URL_COMPONENTSW parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 443) return 0;
        const wstring host(parts.lpszHostName, parts.dwHostNameLength);
        if (host != L"github.com" && host != L"api.github.com") return 0;
        HINTERNET session = WinHttpOpen(L"PhoneRecord/" L"2.7" L" (release updater)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
        HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", wstring(parts.lpszUrlPath, parts.dwUrlPathLength).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
        DWORD status = 0;
        if (request) {
            bool allowed = false;
            { std::lock_guard<std::mutex> guard(mutex_); if (!stopped_) { request_ = request; allowed = true; } }
            if (allowed) {
                DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP, redirects = 5, autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
                WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
                WinHttpSetOption(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));
                WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
                const ULONGLONG deadline = GetTickCount64() + (destination.empty() ? 30000 : 180000);
                if (WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1), NULL, 0, 0, 0) && WinHttpReceiveResponse(request, NULL)) {
                    DWORD size = sizeof(status);
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                    if (status == 200) {
                        std::ofstream output;
                        if (!destination.empty()) output.open(destination, std::ios::binary | std::ios::trunc);
                        bool ok = destination.empty() || !!output; size_t total = 0; char buffer[65536]; DWORD received = 0;
                        while (ok) {
                            if (GetTickCount64() >= deadline || !WinHttpReadData(request, buffer, sizeof(buffer), &received)) { ok = false; break; }
                            if (!received) break;
                            total += received; if (total > limit) { ok = false; break; }
                            if (destination.empty()) bytes.append(buffer, received); else { output.write(buffer, received); if (!output) ok = false; }
                            if (progress) progress(total);
                        }
                        if (output.is_open()) { output.flush(); ok = ok && !!output; output.close(); }
                        if (!ok) status = 0;
                    }
                }
                std::lock_guard<std::mutex> guard(mutex_);
                if (request_ == request) { WinHttpCloseHandle(request); request_ = NULL; }
            } else WinHttpCloseHandle(request);
        }
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if (status != 200 && !destination.empty()) DeleteFileW(destination.c_str());
        return status;
    }

private:
    /// Serializes cancellation against request ownership and double-close.
    std::mutex mutex_;
    /// Only the active request is closed by cancellation; its parents belong to get().
    HINTERNET request_ = NULL;
    /// Permanently disable new requests during shutdown.
    bool stopped_ = false;
};

/// Immutable UI snapshot copied under the update mutex.
struct UpdateState {
    /// Resource key describing the most recent operation, independent of recording health.
    string status = "update.idle";
    /// Signed newer release; empty means no verified candidate.
    UpdateManifest manifest;
    /// Exact signed bytes retained for helper-side verification.
    string signedManifest, signature;
    /// Release notes are displayed as plain untrusted text, never executed or rendered as HTML.
    string notes;
    /// Ready staged executable; set only after complete signature/hash verification.
    wstring staged;
    /// One serialized network operation at a time.
    bool busy = false;
    /// Download percentage, or zero outside a transfer.
    int percent = 0;
    /// Local completion timestamp of the most recent metadata check in this process.
    string checkedAt;
    /// Monotonic UI invalidation marker, including preference changes.
    uint64_t revision = 0;
};

/// Owned update scheduler and worker; no calls to phone APIs or UI controls.
class UpdateService {
public:
    /// Cancel all network I/O and join the owned worker before its window disappears.
    ~UpdateService() { stop(); }

    /// Load per-config preferences/cached signed availability, then start the periodic worker.
    void start(HWND window, const wstring& config, bool offline) {
        window_ = window; preferencesPath_ = config + L".updates"; cachePath_ = config + L".update-cache"; cleanupPath_ = config + L".update-cleanup"; offline_ = offline;
        preferences_.load(preferencesPath_);
        string cached;
        if (updateRead(cachePath_, cached, 12288)) {
            const size_t split = cached.find("\n\n"); UpdateManifest manifest;
            if (split != string::npos) {
                const string bytes = cached.substr(0, split + 1), signature = cached.substr(split + 2);
                if (updateSignature(bytes, signature) && manifest.parse(bytes) && updateNewer(manifest.version)) {
                    state_.manifest = manifest; state_.signedManifest = bytes; state_.signature = signature; state_.status = "update.available";
                }
            }
        }
        if (offline_) state_.status = "update.offline";
        string result;
        if (updateRead(config + L".update-result", result) && (result == "update.installed\n" || result == "update.restored\n" || result == "update.install_failed\n" || result == "update.restore_failed\n" || result == "update.backup_exists\n")) {
            state_.status = result.substr(0, result.size() - 1);
        }
        schedule_.configure(GetTickCount64(), preferences_.hours, true);
        worker_ = std::thread(&UpdateService::loop, this);
    }

    /// Wake and cancel a request; safe to call repeatedly during normal shutdown.
    void stop() {
        { std::lock_guard<std::mutex> guard(mutex_); stopped_ = true; }
        http_.cancel(); wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (!preserveStage_) cleanupStage(state_.staged);
    }

    /// Transfer staging ownership to the already validated installer before closing the UI.
    void preserveStaging() { preserveStage_ = true; }

    /// Freeze the authenticated candidate so a periodic check cannot remove it during installation.
    bool reserveInstall(const UpdateState& candidate) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopped_ || installing_ || state_.busy || check_ || download_ || state_.staged.empty() || state_.staged != candidate.staged || state_.manifest.sha256 != candidate.manifest.sha256) return false;
        installing_ = true; return true;
    }
    /// Resume update checks if call admission or helper preparation prevented the installation.
    void cancelInstall() { std::lock_guard<std::mutex> guard(mutex_); installing_ = false; wake_.notify_all(); }

    /// Copy update state without exposing mutable objects across threads.
    UpdateState state() const { std::lock_guard<std::mutex> guard(mutex_); return state_; }
    /// Copy persisted update preferences for rendering on the UI thread.
    UpdatePreferences preferences() const { std::lock_guard<std::mutex> guard(mutex_); return preferences_; }

    /// Save independent update preferences immediately and reset the periodic schedule.
    bool configure(int hours, bool balloons) {
        if (hours != 0 && hours != 1 && hours != 6 && hours != 12 && hours != 24) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        UpdatePreferences candidate = preferences_; candidate.hours = hours; candidate.balloons = balloons;
        if (!candidate.save(preferencesPath_)) return false;
        preferences_ = candidate; schedule_.configure(GetTickCount64(), hours); ++state_.revision; wake_.notify_all(); return true;
    }

    /// Persist notification deduplication only after Windows accepted a balloon request.
    bool markNotified(const string& version) {
        std::lock_guard<std::mutex> guard(mutex_); UpdatePreferences candidate = preferences_; candidate.notified = version;
        if (!candidate.save(preferencesPath_)) return false;
        preferences_ = candidate; return true;
    }

    /// Coalesce repeated manual checks and downloads rather than queueing network work.
    void request(bool download = false) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopped_ || installing_ || state_.busy || check_ || download_) return;
        if (offline_) { state_.status = "update.offline"; publish(); return; }
        if (!*updatePinnedKey()) { state_.status = "update.no_key"; publish(); return; }
        if (GetTickCount64() < rateLimitUntil_) { state_.status = "update.rate_limit"; publish(); return; }
        if (download && (state_.manifest.version.empty() || !state_.staged.empty())) return;
        download_ = download; check_ = !download; wake_.notify_all();
    }

    /// Fixed release URL constructed from validated numeric version, never from a remote executable URL.
    static wstring releaseUrl(const string& version, const wchar_t* asset) {
#ifdef _WIN64
        const std::map<wstring, wstring> names = {{L"PhoneRecord.exe", L"PhoneRecord-x64.exe"}, {L"PhoneRecord.exe.sha256", L"PhoneRecord-x64.exe.sha256"},
            {L"update-manifest.txt", L"update-manifest-x64.txt"}, {L"update-manifest.sig", L"update-manifest-x64.sig"}};
        const auto selected = names.find(asset);
        if (selected == names.end()) return L"";
        return L"https://github.com/ChenRanReal/PhoneRecord/releases/download/v" + utf8ToWide(version) + L"/" + selected->second;
#else
        return L"https://github.com/ChenRanReal/PhoneRecord/releases/download/v" + utf8ToWide(version) + L"/" + asset;
#endif
    }

#ifdef PHONERECORD_UPDATE_TESTING
    /// Advance the real worker's deadline without waiting hours; absent from production.
    void testDueSoon() { std::lock_guard<std::mutex> guard(mutex_); schedule_.next = GetTickCount64() + 20; wake_.notify_all(); }
#endif

private:
    /// Remove only this download's four known files, never recurse or touch recordings/configuration.
    static void cleanupStage(const wstring& staged) {
        if (staged.empty()) return;
        const wstring directory = staged.substr(0, staged.find_last_of(L"\\/"));
        for (const wchar_t* name : {L"PhoneRecord.exe", L"PhoneRecord-updater.exe", L"update-manifest.txt", L"update-manifest.sig"}) DeleteFileW((directory + L"\\" + name).c_str());
        RemoveDirectoryW(directory.c_str());
    }

    /// Retry a completed helper's known temporary files; validate namespace and never recurse.
    void cleanupPrevious() {
        string bytes; if (!updateRead(cleanupPath_, bytes, 2048)) return;
        wchar_t temporary[MAX_PATH]{}; if (!GetTempPathW(MAX_PATH, temporary)) return;
        const wstring directory = utf8ToWide(bytes), prefix = wstring(temporary) + L"PhoneRecord-update-";
        if (directory.size() != prefix.size() + 38 || directory.compare(0, prefix.size(), prefix)) return;
        GUID guid{}; if (FAILED(CLSIDFromString(directory.substr(prefix.size()).c_str(), &guid))) return;
        cleanupStage(directory + L"\\PhoneRecord.exe");
        if (GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) DeleteFileW(cleanupPath_.c_str());
    }
    /// Send only a revision notice; snapshots retain ownership of all payload data.
    void publish() { ++state_.revision; if (window_) PostMessageW(window_, WM_APP + 24, 0, 0); }
    /// Report a bounded resource key without overwriting previously verified availability.
    void failure(const char* status) { std::lock_guard<std::mutex> guard(mutex_); state_.status = status; state_.busy = false; publish(); LOGW("UPDATE_OPERATION_FAILED code=%s", status); }

    /// Query latest stable release and verify signed metadata before displaying an update badge.
    void check() {
        string release;
        DWORD status = http_.get(L"https://api.github.com/repos/ChenRanReal/PhoneRecord/releases/latest", release, 512 * 1024);
        if (status == 403 || status == 429) {
            { std::lock_guard<std::mutex> guard(mutex_); rateLimitUntil_ = GetTickCount64() + 3600000; }
            failure("update.rate_limit"); return;
        }
        if (status != 200 && status != 404) { failure("update.network_error"); return; }
        const string tag = status == 200 ? jsonField(release, "tag_name") : "";
        std::array<unsigned, 3> parts{};
        if (status == 200 && (tag.empty() || tag[0] != 'v' || !updateVersion(tag.substr(1), parts))) { failure("update.invalid_release"); return; }
        if (status == 404 || !updateNewer(tag.substr(1))) {
            std::lock_guard<std::mutex> guard(mutex_); cleanupStage(state_.staged); state_.manifest = {}; state_.signedManifest.clear(); state_.signature.clear(); state_.notes.clear(); state_.staged.clear();
            DeleteFileW(cachePath_.c_str()); state_.status = "update.latest"; state_.busy = false; publish(); return;
        }
        std::set<string> assets;
        size_t position = 0;
        while ((position = release.find('"', position)) != string::npos) {
            string field; if (!readJsonString(release, position, field)) break;
            size_t value = release.find_first_not_of(" \t\r\n", position);
            if (field != "browser_download_url" || value == string::npos || release[value] != ':') continue;
            value = release.find_first_not_of(" \t\r\n", value + 1); string url;
            if (value != string::npos && readJsonString(release, value, url)) assets.insert(url);
        }
        for (const wchar_t* asset : {L"PhoneRecord.exe", L"PhoneRecord.exe.sha256", L"update-manifest.txt", L"update-manifest.sig"}) {
            if (!assets.count(wideToUtf8(releaseUrl(tag.substr(1), asset)))) { failure("update.incomplete_release"); return; }
        }
        string bytes, signature; UpdateManifest manifest;
        if (http_.get(releaseUrl(tag.substr(1), L"update-manifest.txt"), bytes, 4096) != 200 ||
            http_.get(releaseUrl(tag.substr(1), L"update-manifest.sig"), signature, 8192) != 200) { failure("update.incomplete_release"); return; }
        if (!updateSignature(bytes, signature) || !manifest.parse(bytes) || manifest.version != tag.substr(1)) { failure("update.invalid_signature"); return; }
        const bool cached = updateWrite(cachePath_, bytes + "\n" + signature);
        std::lock_guard<std::mutex> guard(mutex_);
        if (state_.manifest.version != manifest.version || state_.manifest.sha256 != manifest.sha256) { cleanupStage(state_.staged); state_.staged.clear(); }
        state_.manifest = manifest; state_.signedManifest = bytes; state_.signature = signature; state_.notes = jsonField(release, "body");
        state_.status = cached ? "update.available" : "update.cache_error"; state_.busy = false; publish();
        LOGI("UPDATE_AVAILABLE version=%s cached=%d", manifest.version.c_str(), cached);
    }

    /// Download to an exclusive random staging directory and authenticate the complete executable.
    void download() {
        UpdateState candidate = state();
        wchar_t temporary[MAX_PATH]{}; if (!GetTempPathW(MAX_PATH, temporary)) { failure("update.write_error"); return; }
        GUID guid{}; wchar_t id[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) { failure("update.write_error"); return; }
        const wstring directory = wstring(temporary) + L"PhoneRecord-update-" + id;
        if (!CreateDirectoryW(directory.c_str(), NULL)) { failure("update.write_error"); return; }
        const wstring path = directory + L"\\PhoneRecord.exe", pending = path + L".part"; string unused;
        const DWORD status = http_.get(releaseUrl(candidate.manifest.version, L"PhoneRecord.exe"), unused, static_cast<size_t>(candidate.manifest.size), pending, [&](size_t size) {
            std::lock_guard<std::mutex> guard(mutex_); const int percent = static_cast<int>(size * 100 / candidate.manifest.size);
            if (state_.percent != percent) { state_.percent = percent; publish(); }
        });
        const bool valid = status == 200 && updateSignature(candidate.signedManifest, candidate.signature) && updateVerifyFile(pending, candidate.manifest);
        if (!valid || !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) ||
            !updateWrite(directory + L"\\update-manifest.txt", candidate.signedManifest) || !updateWrite(directory + L"\\update-manifest.sig", candidate.signature)) {
            DeleteFileW(pending.c_str()); DeleteFileW(path.c_str()); DeleteFileW((directory + L"\\update-manifest.txt").c_str()); DeleteFileW((directory + L"\\update-manifest.sig").c_str()); RemoveDirectoryW(directory.c_str());
            failure(status == 200 ? "update.invalid_download" : "update.network_error"); return;
        }
        std::lock_guard<std::mutex> guard(mutex_); state_.staged = path; state_.status = "update.ready"; state_.busy = false; state_.percent = 100; publish();
        LOGI("UPDATE_DOWNLOAD_VERIFIED version=%s", candidate.manifest.version.c_str());
    }

    /// Wait on explicit requests or due time; waking late runs one check and never a catch-up burst.
    void loop() {
        int cleanupAttempts = 0;
        while (true) {
            if (cleanupAttempts++ < 15) cleanupPrevious();
            bool transfer = false;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                wake_.wait_for(guard, std::chrono::seconds(1), [&] { return stopped_ || check_ || download_; });
                if (stopped_) break;
                if (installing_) continue;
                const bool due = schedule_.due(GetTickCount64(), preferences_.hours);
                if (!check_ && !download_ && (!due || offline_ || !*updatePinnedKey() || GetTickCount64() < rateLimitUntil_)) continue;
                transfer = download_; check_ = download_ = false; state_.busy = true; state_.percent = 0;
                state_.status = transfer ? "update.downloading" : "update.checking"; publish();
            }
            try { if (transfer) download(); else { check(); std::lock_guard<std::mutex> guard(mutex_); state_.checkedAt = diagnosticTime(); publish(); } }
            catch (...) { failure("update.operation_error"); }
        }
    }

    /// Protect preferences, schedule, request flags and snapshots.
    mutable std::mutex mutex_;
    /// Wake for changes or shutdown without blocking the GUI thread.
    std::condition_variable wake_;
    /// Single serialized background network worker.
    std::thread worker_;
    /// Cancelable transport owned by this service.
    UpdateHttp http_;
    /// UI notice target valid until stop() returns.
    HWND window_ = NULL;
    /// Independent per-config metadata paths.
    wstring preferencesPath_, cachePath_, cleanupPath_;
    /// Update UI preferences, cached status and monotonic schedule.
    UpdatePreferences preferences_;
    UpdateState state_;
    UpdateSchedule schedule_;
    /// Requests are coalesced rather than accumulated.
    bool check_ = false, download_ = false, stopped_ = false, offline_ = false;
    /// Only the GUI sets this after helper acknowledgement, before joining the worker.
    bool preserveStage_ = false;
    /// Prevent candidate replacement between operator confirmation and helper ownership transfer.
    bool installing_ = false;
    /// Conservative one-hour cooldown after a GitHub 403/429; manual checks also respect it.
    uint64_t rateLimitUntil_ = 0;
};
