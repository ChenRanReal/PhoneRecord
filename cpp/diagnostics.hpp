#pragma once

#include <cmath>
#include <iomanip>

/// Product version included in the window, logs and diagnostic reports.
static const char* const kProductVersion = "2.6.0";

/// Format a local wall-clock timestamp with millisecond precision.
static std::string diagnosticTime() {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             value.wYear, value.wMonth, value.wDay, value.wHour,
             value.wMinute, value.wSecond, value.wMilliseconds);
    return buffer;
}

/// Escape control characters and suppress credentials or long numeric identifiers.
static std::string redactDiagnostic(std::string text) {
    static const std::regex secrets(
        "((?:auth|nonce|encoded|password|phone_pass)[=:])[^ &;\\r\\n\\\\]+",
        std::regex_constants::icase);
    static const std::regex cookie("((?:cookie|nonce)=\\[)[^\\]]*\\]", std::regex_constants::icase);
    static const std::regex numbers("[0-9]{7,}");
    text = std::regex_replace(text, cookie, "$1redacted]");
    text = std::regex_replace(text, secrets, "$1<redacted>");
    text = std::regex_replace(text, numbers, "<number>");
    std::string clean;
    clean.reserve(text.size());
    for (unsigned char value : text) {
        if (value == '\r') clean += "\\r";
        else if (value == '\n') clean += "\\n";
        else if (value == '\t') clean += " ";
        else if (value >= 32) clean += static_cast<char>(value);
    }
    return clean;
}

/// One bounded, redacted diagnostic event, suitable for local export.
struct DiagnosticEntry {
    /// Monotonic event sequence used by the live viewer.
    uint64_t sequence = 0;
    /// Local wall-clock timestamp including milliseconds.
    std::string time;
    /// Stable severity: INFO, DEBUG, WARN, ERROR.
    std::string level;
    /// Redacted single-line event text, beginning with a stable event code.
    std::string text;
    /// Native worker identifier useful when interleaved operations are inspected.
    DWORD thread = 0;
};

/// Thread-safe log with a 2,000-entry memory ring and three rotated 2 MiB files.
class DiagnosticLog {
public:
    /// Append one redacted event to the memory viewer and the bounded disk log.
    void write(const char* level, const std::string& message) {
        std::lock_guard<std::mutex> guard(mutex_);
        DiagnosticEntry event;
        event.sequence = ++sequence_;
        event.time = diagnosticTime();
        event.level = level;
        event.text = redactDiagnostic(message);
        event.thread = GetCurrentThreadId();
        entries_.push_back(event);
        while (entries_.size() > 2000) entries_.pop_front();
        if (!rotate()) return;
        std::ofstream output("PhoneRecord.log", std::ios::binary | std::ios::app);
        if (!output) { storageError_ = "LOG_WRITE_FAILED"; return; }
        output << event.time << " [" << event.level << "] [T" << event.thread << "] " << event.text << "\n";
        output.flush();
        if (!output) storageError_ = "LOG_WRITE_FAILED";
    }

    /// Return a snapshot without retaining the log mutex while the UI renders.
    std::vector<DiagnosticEntry> entries() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return std::vector<DiagnosticEntry>(entries_.begin(), entries_.end());
    }

    /// Return the newest sequence so unchanged log views need no rebuilding.
    uint64_t sequence() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return sequence_;
    }

    /// Report a disk failure independently of the disk log itself.
    std::string storageError() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return storageError_;
    }

private:
    /// Rotate only this application's exact log filenames before appending.
    bool rotate() {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(L"PhoneRecord.log", GetFileExInfoStandard, &data)) return true;
        if (data.nFileSizeHigh == 0 && data.nFileSizeLow < 2 * 1024 * 1024) return true;
        DeleteFileW(L"PhoneRecord.log.3");
        MoveFileExW(L"PhoneRecord.log.2", L"PhoneRecord.log.3", MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(L"PhoneRecord.log.1", L"PhoneRecord.log.2", MOVEFILE_REPLACE_EXISTING);
        if (!MoveFileExW(L"PhoneRecord.log", L"PhoneRecord.log.1", MOVEFILE_REPLACE_EXISTING)) {
            storageError_ = "LOG_ROTATE_FAILED"; return false;
        }
        return true;
    }

    /// Protect the ring, sequence and storage status together.
    mutable std::mutex mutex_;
    /// Recent diagnostic events only; existing legacy logs are not re-exported.
    std::deque<DiagnosticEntry> entries_;
    /// Last assigned sequence number.
    uint64_t sequence_ = 0;
    /// Last observable disk logging failure.
    std::string storageError_;
};

/// Product log shared by background workers and the native viewer.
static DiagnosticLog g_diagnosticLog;

/// Observable recording stages; transport progress is separate from audio quality.
enum class RecordingStage { Waiting, Event, Command, Control, Reply, Media, File, Complete, Failed };

/// Current health of one independently checked subsystem.
struct HealthResult {
    /// Zero is not checked, one healthy, two failed, three currently checking.
    int status = 0;
    /// Stable summary code and last completed check time.
    std::string code = "NOT_CHECKED", checkedAt;
};

/// A consistent UI/report snapshot, containing no phone credentials or caller names.
struct DiagnosticSnapshot {
    /// Independent results prevent a past call failure from colouring successful checks red.
    HealthResult localHealth, phoneHealth;
    /// Valid RTP packets belonging to the represented call rather than the process lifetime.
    uint64_t attemptPackets = 0;
    /// Whether the TCP recording control listener actually bound successfully.
    bool controlListening = false;
    /// Whether this process actually owns the configured Action URL listener.
    bool eventListening = false;
    /// Whether an authenticated phone page was verified.
    bool authenticated = false;
    /// Whether an explicit phone operation is still executing.
    bool busy = false;
    /// Whether an established call has an outstanding recording request.
    bool callActive = false;
    /// Current attempt's highest observed stage or terminal failure.
    RecordingStage stage = RecordingStage::Waiting;
    /// Last stable diagnostic code, mapped to operator guidance in the UI.
    std::string code = "STARTING";
    /// Safe supplementary information such as elapsed time and port numbers.
    std::string detail;
    /// Local timestamp of the most recent progress or failure.
    std::string changedAt;
    /// Number of recording attempts since process start.
    uint64_t attempts = 0;
    /// Total accepted Action URL callbacks.
    uint64_t events = 0;
    /// Total accepted TCP control connections.
    uint64_t connections = 0;
    /// Total parsed control frames.
    uint64_t frames = 0;
    /// Total successfully transmitted control responses.
    uint64_t responses = 0;
    /// Total valid RTP packets received, independent of later storage success.
    uint64_t packets = 0;
    /// Total PCM bytes successfully written to WAV files.
    uint64_t pcmBytes = 0;
    /// Completed WAV files, including suspect audio that needs review.
    uint64_t files = 0;
    /// Most recent signal category, never an assertion of real two-way speech.
    std::string quality = "unknown";
    /// Session currently represented by the overview; older timeouts cannot overwrite it.
    std::string sessionTag;
    /// Explicit trigger source: manual, phone_event or device_control.
    std::string triggerSource = "none";
    /// Most recent completed file's peak amplitude.
    int peak = 0;
    /// Most recent completed file's RMS amplitude.
    double rms = 0;
    /// Whether this recording attempt accepted a TCP connection.
    bool attemptTcp = false;
    /// Whether this attempt actually sent the complete HTTP Start request.
    bool attemptHttp = false;
    /// Whether this recording attempt sent its Start response successfully.
    bool attemptReply = false;
    /// Whether this recording attempt received valid RTP.
    bool attemptRtp = false;
    /// Whether this recording attempt created a WAV file.
    bool attemptWav = false;
};

/// Mutex protecting the complete diagnostic snapshot.
static std::mutex g_diagnosticMutex;
/// Current mutable diagnostic state; read through diagnosticSnapshot().
static DiagnosticSnapshot g_diagnosticState;
/// Process shutdown signal shared by owned services and UI workers.
static std::atomic<bool> g_stopping{false};
/// Early call-ended signal lets an in-flight Start wait yield to Stop promptly.
static std::atomic<bool> g_callEnded{false};
/// Set by the test-only offline mode to prohibit phone HTTP traffic.
static bool g_offline = false;

/// Apply a short state update under the snapshot mutex; never call logging inside it.
static void diagnosticUpdate(const std::function<void(DiagnosticSnapshot&)>& change) {
    std::lock_guard<std::mutex> guard(g_diagnosticMutex);
    change(g_diagnosticState);
    g_diagnosticState.changedAt = diagnosticTime();
}

/// Copy current diagnostics for native UI rendering or export.
static DiagnosticSnapshot diagnosticSnapshot() {
    std::lock_guard<std::mutex> guard(g_diagnosticMutex);
    return g_diagnosticState;
}

/// Complete one independent health check without altering the recording state machine.
static void diagnosticHealth(bool phone, bool healthy, const char* code) {
    diagnosticUpdate([&](DiagnosticSnapshot& state) {
        auto& result = phone ? state.phoneHealth : state.localHealth;
        result.status = healthy ? 1 : 2; result.code = code; result.checkedAt = diagnosticTime();
    });
}

/// Live desktop preferences; the network configuration remains immutable until restart.
static std::atomic<int> g_healthIntervalSec{60};
/// Last call boundary; periodic phone checks wait for an idle quiet interval.
static std::atomic<ULONGLONG> g_lastCallActivity{0};
static std::atomic<bool> g_trayNotifications{false};
static std::atomic<int> g_closeAction{0}; // 0 system tray, 1 taskbar, 2 exit.

/// Set a visible stage and stable event code, with credential-safe details.
static void diagnosticStage(RecordingStage stage, const char* code, const std::string& detail = "") {
    diagnosticUpdate([&](DiagnosticSnapshot& state) {
        state.stage = stage;
        state.code = code;
        state.detail = redactDiagnostic(detail);
    });
}

/// Reset per-attempt transport flags without discarding lifetime counters.
static void diagnosticBegin(const char* origin) {
    diagnosticUpdate([&](DiagnosticSnapshot& state) {
        ++state.attempts;
        state.stage = RecordingStage::Event;
        state.code = "ATTEMPT_STARTED";
        state.detail = origin;
        state.triggerSource = origin;
        state.attemptHttp = state.attemptTcp = state.attemptReply = state.attemptRtp = state.attemptWav = false;
        state.quality = "unknown"; state.attemptPackets = 0;
        state.sessionTag.clear();
    });
}

/// Format a short session fingerprint without writing the original identifier.
static std::string sessionFingerprint(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char byte : value) hash = (hash ^ byte) * 16777619u;
    char output[16];
    snprintf(output, sizeof(output), "s-%04x-%04x", (hash >> 16) & 0xffff, hash & 0xffff);
    return output;
}

/// Send the entire response within a bounded wait, including nonblocking sockets.
static bool sendComplete(SOCKET socket, const std::string& payload, int timeoutMs = 2500, size_t* writtenBytes = NULL) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    size_t offset = 0;
    if (writtenBytes) *writtenBytes = 0;
    while (offset < payload.size() && !g_stopping) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { WSASetLastError(WSAETIMEDOUT); return false; }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        timeval wait{};
        wait.tv_sec = static_cast<long>((deadline - now) / 1000);
        wait.tv_usec = static_cast<long>(((deadline - now) % 1000) * 1000);
        int ready = select(0, NULL, &writable, NULL, &wait);
        if (ready == 0) { WSASetLastError(WSAETIMEDOUT); return false; }
        if (ready == SOCKET_ERROR) return false;
        int sent = send(socket, payload.data() + offset,
                        static_cast<int>(std::min<size_t>(payload.size() - offset, 65536)), 0);
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
        if (writtenBytes) *writtenBytes = offset;
    }
    if (offset != payload.size() && g_stopping) WSASetLastError(WSAESHUTDOWN);
    return offset == payload.size();
}

/// Incremental 8 kHz mono PCM analysis without retaining call audio in memory.
class PcmQuality {
public:
    /// Include samples that were successfully written; complete seconds drive variation.
    void add(const int16_t* samples, int count) {
        for (int index = 0; index < count; ++index) {
            const int value = samples[index];
            peak_ = std::max(peak_, std::abs(value));
            const double energy = static_cast<double>(value) * value;
            energy_ += energy;
            secondEnergy_ += energy;
            ++samples_;
            if (++secondSamples_ == 8000) {
                const int centiRms = static_cast<int>(std::llround(std::sqrt(secondEnergy_ / 8000) * 100));
                if (fullSeconds_ == 0) firstSecondRms_ = centiRms;
                else if (centiRms != firstSecondRms_) varying_ = true;
                ++fullSeconds_;
                secondSamples_ = 0;
                secondEnergy_ = 0;
            }
        }
    }

    /// Return the amplitude-and-variation category; provenance must be checked separately.
    std::string category() const {
        if (samples_ == 0) return "empty";
        if (peak_ * 100.0 / 32768 <= 5 || rms() <= 30) return "silence";
        if (fullSeconds_ < 2) return "short";
        if (!varying_) return "constant";
        return "audio";
    }

    /// Return the full-file peak amplitude.
    int peak() const { return peak_; }
    /// Return the full-file RMS amplitude, including a partial final second.
    double rms() const { return samples_ ? std::sqrt(energy_ / samples_) : 0; }
    /// Return duration derived from successfully written PCM samples.
    double duration() const { return samples_ / 8000.0; }
    /// Return the number of complete seconds available for variation analysis.
    uint64_t fullSeconds() const { return fullSeconds_; }
    /// Return whether at least two complete seconds have distinct RMS values.
    bool varying() const { return varying_; }

private:
    /// Full-file PCM sample count.
    uint64_t samples_ = 0;
    /// Complete seconds analysed independently of the partial tail.
    uint64_t fullSeconds_ = 0;
    /// Full-file squared-sample sum.
    double energy_ = 0;
    /// Current partial second's squared-sample sum.
    double secondEnergy_ = 0;
    /// Samples accumulated for the current second.
    int secondSamples_ = 0;
    /// Reference first-second RMS in hundredths.
    int firstSecondRms_ = 0;
    /// Full-file peak magnitude.
    int peak_ = 0;
    /// Whether a later complete second differs from the reference.
    bool varying_ = false;
};
