#pragma once

/// Find the earliest supported control delimiter, including a binary NUL.
static size_t firstDelimiter(const string& buffer) {
    size_t result = string::npos;
    for (char delimiter : {'\r', '\n', '\0'}) {
        const size_t position = buffer.find(delimiter);
        if (position != string::npos && (result == string::npos || position < result)) result = position;
    }
    return result;
}

/// Parse bounded ampersand key/value fields without interpreting caller names as commands.
static std::map<string, string> parseKv(const string& text) {
    std::map<string, string> result;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('&', start);
        const string part = text.substr(start, end == string::npos ? string::npos : end - start);
        const size_t equal = part.find('=');
        if (equal != string::npos) result[trim(part.substr(0, equal))] = trim(part.substr(equal + 1));
        if (end == string::npos) break;
        start = end + 1;
    }
    return result;
}

/// Quote user-owned call ledger fields without corrupting commas, quotes or newlines.
static string ledgerCell(const string& value) {
    string result = "\"";
    for (char character : value) result += character == '"' ? "\"\"" : string(1, character);
    return result + "\"";
}

/// One owned UDP-to-WAV recording; technical logs contain no caller names or numbers.
class RecordSession {
public:
    /// Adopt the UDP socket and immutable phone parameters for this session.
    RecordSession(const string& id, const std::map<string, string>& params,
                  const string& codec, int port, SOCKET socket, const string& controlPeer)
        : id_(id), tag_(sessionFingerprint(id)), codec_(codec), port_(port), socket_(socket), controlPeer_(controlPeer) {
        const auto field = [&](const char* key) { const auto it = params.find(key); return it == params.end() ? string() : it->second; };
        localNumber_ = field("local_number");
        remoteNumber_ = field("remote_number");
        direction_ = field("dir");
        name_ = field("remote_name");
        if (name_.empty()) name_ = g_directory.lookup(remoteNumber_);
    }

    /// Stop and join before releasing the adopted socket.
    ~RecordSession() { stop("SHUTDOWN"); join(); cleanup(); }

    /// Start the sole UDP worker; the Recorder retains ownership until join().
    void start() { worker_ = std::thread(&RecordSession::run, this); }

    /// Request cooperative termination without closing a socket underneath recvfrom().
    void stop(const char* reason) {
        std::lock_guard<std::mutex> guard(reasonMutex_);
        if (!stop_) { reason_ = reason; stop_ = true; }
    }

    /// Join the worker once it has completed its file and metadata writes.
    void join() { if (worker_.joinable()) worker_.join(); }
    /// Return the allocated RTP port.
    int port() const { return port_; }
    /// Return true only after file closure, metadata, ledger and socket cleanup.
    bool stopped() const { return finished_; }
    /// Return whether this session actually received and wrote a valid RTP packet.
    bool hasMedia() const { return packetCount_ > 0; }

private:
    /// Receive bounded RTP datagrams, track losses, and write only supported payloads.
    void run() {
        const ULONGLONG started = GetTickCount64();
        ULONGLONG lastPacket = started, lastSummary = started;
        uint8_t buffer[4096];
        int16_t pcm[4096];
        while (!stop_ && !g_stopping) {
            fd_set readable;
            FD_ZERO(&readable); FD_SET(socket_, &readable);
            timeval wait{0, 200000};
            int ready = select(0, &readable, NULL, NULL, &wait);
            const ULONGLONG now = GetTickCount64();
            if (ready == SOCKET_ERROR) { setWorkerReason("RTP_RECEIVE_ERROR"); break; }
            if (!ready) {
                const ULONGLONG limit = packetCount_ ? static_cast<ULONGLONG>(g_cfg.idleTimeoutSec) * 1000 : 10000;
                if (now - lastPacket >= limit) { setWorkerReason(packetCount_ ? "RTP_IDLE_TIMEOUT" : "RTP_NOT_RECEIVED"); break; }
                continue;
            }
            sockaddr_in source{};
            int length = sizeof(source);
            int received = recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&source), &length);
            if (received <= 0) { if (received < 0) setWorkerReason("RTP_RECEIVE_ERROR"); continue; }
            RtpPacket rtp;
            if (!parseRtp(buffer, received, rtp) || (rtp.payloadType != 0 && rtp.payloadType != 8) || rtp.payloadSize > 4096) {
                if (++invalid_ == 1) LOGW("RTP_INVALID session=%s bytes=%d", tag_.c_str(), received);
                continue;
            }
            const string sourceIp = inet_ntoa(source.sin_addr);
            if (!peerLocked_) {
                // Only the TCP peer may choose the first RTP source; do not lock on junk UDP.
                if (sourceIp != controlPeer_) { ++invalid_; continue; }
                peer_ = source;
                peerLocked_ = true;
                provenance_ = sourceIp == g_cfg.phoneIp && sourceIp != "127.0.0.1" ? "configured_phone" : "test_or_other_peer";
                LOGI("RTP_FIRST session=%s source=%s:%u pt=%d payload=%d elapsed_ms=%llu provenance=%s",
                     tag_.c_str(), sourceIp.c_str(), ntohs(source.sin_port), rtp.payloadType, rtp.payloadSize,
                     now - started, provenance_.c_str());
            } else if (source.sin_addr.s_addr != peer_.sin_addr.s_addr || source.sin_port != peer_.sin_port) {
                ++invalid_; continue;
            }
            // Observe source switches even if the existing sequence filter rejects the packet.
            if (ssrcSeen_ && rtp.ssrc != observedSsrc_) ++ssrcChanges_;
            ssrcSeen_ = true; observedSsrc_ = rtp.ssrc;
            int sequenceDelta = 0;
            if (sequenceSeen_) {
                const int delta = static_cast<int16_t>(static_cast<uint16_t>(rtp.sequence - lastSequence_));
                sequenceDelta = delta;
                if (delta == 0) { ++duplicates_; continue; }
                if (delta < 0) { ++outOfOrder_; continue; }
                if (delta > 1) lost_ += delta - 1;
            }
            // Compare only consecutive accepted packets of the same source. A skipped
            // sequence already has separate loss evidence; do not count it as a source gap.
            if (sequenceSeen_ && sequenceDelta == 1 && rtp.ssrc == lastSsrc_) {
                const uint32_t step = rtp.timestamp - lastTimestamp_;
                if (step != lastPayloadSize_) {
                    ++timestampDiscontinuities_;
                    if (step < 0x80000000U && step > lastPayloadSize_) timestampGapSamples_ += step - lastPayloadSize_;
                    else ++timestampOverlapOrReset_;
                    if (timestampDiscontinuities_ <= 8)
                        LOGW("RTP_CLOCK_DISCONTINUITY session=%s packet=%llu step_samples=%lu expected_samples=%lu arrival_ms=%llu",
                             tag_.c_str(), packetCount_.load() + 1, static_cast<unsigned long>(step),
                             static_cast<unsigned long>(lastPayloadSize_), now - started);
                }
            }
            lastTimestamp_ = rtp.timestamp; lastSsrc_ = rtp.ssrc; lastPayloadSize_ = static_cast<uint32_t>(rtp.payloadSize);
            sequenceSeen_ = true;
            lastSequence_ = rtp.sequence;
            if (packetCount_) maxGapMs_ = std::max(maxGapMs_, now - lastPacket);
            lastPacket = now;
            const bool ulaw = rtp.payloadType == 0;
            for (int index = 0; index < rtp.payloadSize; ++index)
                pcm[index] = ulaw ? g_ulaw[buffer[rtp.payloadOffset + index]] : g_alaw[buffer[rtp.payloadOffset + index]];
            ++packetCount_;
            if (packetCount_ == 1 || (packetCount_ <= 100 && packetCount_ % 5 == 0))
                LOGD("RTP_TIMING session=%s packet=%llu arrival_ms=%llu timestamp=%lu payload_samples=%d",
                     tag_.c_str(), packetCount_.load(), now - started, static_cast<unsigned long>(rtp.timestamp), rtp.payloadSize);
            diagnosticUpdate([&](DiagnosticSnapshot& state) {
                ++state.packets;
                if (state.sessionTag == tag_) { ++state.attemptPackets; state.attemptRtp = true; state.stage = RecordingStage::Media; state.code = "RTP_RECEIVED"; }
            });
            if (!wav_.isOpen() && !openOutput()) { setWorkerReason("WAV_OPEN_FAILED"); break; }
            if (!wav_.write(pcm, rtp.payloadSize)) { setWorkerReason("WAV_WRITE_FAILED"); break; }
            quality_.add(pcm, rtp.payloadSize);
            diagnosticUpdate([&](DiagnosticSnapshot& state) {
                state.pcmBytes += rtp.payloadSize * 2;
                if (state.sessionTag != tag_) return;
                state.attemptRtp = state.attemptWav = true;
                if (state.stage != RecordingStage::Failed) state.stage = RecordingStage::File;
                state.code = "RECORDING";
                state.peak = quality_.peak(); state.rms = quality_.rms();
            });
            if (now - lastSummary >= 5000) {
                LOGI("RTP_SUMMARY session=%s packets=%llu lost=%llu invalid=%llu pcm_bytes=%llu max_gap_ms=%llu peak=%d rms=%.2f",
                     tag_.c_str(), packetCount_.load(), lost_, invalid_, wav_.dataBytes(), maxGapMs_, quality_.peak(), quality_.rms());
                lastSummary = now;
            }
        }
        finish();
    }

    /// Coordinate worker-detected completion causes with externally requested Stop.
    void setWorkerReason(const char* reason) {
        std::lock_guard<std::mutex> guard(reasonMutex_); reason_ = reason;
    }

    /// Create a collision-resistant WAV name while retaining the existing list filename convention.
    bool openOutput() {
        if (!ensureDir(g_cfg.recordDir)) { LOGE("RECORD_DIR_CREATE_FAILED win32=%lu", GetLastError()); return false; }
        string base = nowStamp("%Y%m%d-%H%M%S") + "_" + safeFileName(localNumber_) + "_" +
                      safeFileName(remoteNumber_) + "_" + safeFileName(direction_);
        if (!name_.empty()) base += "_" + safeFileName(name_);
        base += "_" + tag_;
        path_ = g_cfg.recordDir + "\\" + base + ".wav";
        if (!wav_.open(path_)) return false;
        fileName_ = base + ".wav";
        diagnosticUpdate([&](DiagnosticSnapshot& state) { if (state.sessionTag == tag_) { state.attemptWav = true; state.stage = RecordingStage::File; state.code = "WAV_OPENED"; } });
        LOGI("WAV_OPENED session=%s udp=%d", tag_.c_str(), port_);
        if (g_uiRecordStart) g_uiRecordStart(name_, remoteNumber_);
        return true;
    }

    /// Finalize the RIFF header and persist a signal/provenance sidecar and compatible ledger.
    void finish() {
        g_lastCallActivity = GetTickCount64();
        string reason;
        { std::lock_guard<std::mutex> guard(reasonMutex_); reason = reason_; }
        const bool closed = wav_.close();
        if (!closed && !fileName_.empty()) reason = "WAV_FINALIZE_FAILED";
        if (!fileName_.empty()) {
            const string category = closed && reason.find("FAILED") == string::npos ? quality_.category() : "io_error";
            std::ofstream metadata(utf8ToWide(path_ + ".quality.json"), std::ios::binary | std::ios::trunc);
            metadata << "{\"quality\":\"" << category << "\",\"source\":\"" << provenance_
                     << "\",\"duration\":" << quality_.duration() << ",\"peak\":" << quality_.peak()
                     << ",\"rms\":" << quality_.rms() << ",\"full_seconds\":" << quality_.fullSeconds()
                     << ",\"varying\":" << (quality_.varying() ? "true" : "false")
                     << ",\"packets\":" << packetCount_ << ",\"lost\":" << lost_
                     << ",\"duplicates\":" << duplicates_ << ",\"out_of_order\":" << outOfOrder_
                     << ",\"invalid\":" << invalid_ << ",\"max_gap_ms\":" << maxGapMs_
                     << ",\"timestamp_discontinuities\":" << timestampDiscontinuities_
                     << ",\"timestamp_gap_samples\":" << timestampGapSamples_
                     << ",\"timestamp_overlap_or_reset\":" << timestampOverlapOrReset_
                     << ",\"ssrc_changes\":" << ssrcChanges_
                     << ",\"reason\":\"" << reason << "\",\"display_name\":" << jsonQuoted(name_) << "}\n";
            metadata.flush();
            if (!metadata) LOGE("QUALITY_SAVE_FAILED session=%s", tag_.c_str());
            appendLedger();
            diagnosticUpdate([&](DiagnosticSnapshot& state) {
                ++state.files; if (state.sessionTag != tag_) return; state.quality = category; state.peak = quality_.peak(); state.rms = quality_.rms();
                state.stage = category == "audio" ? RecordingStage::Complete : RecordingStage::Failed;
                state.code = category == "audio" ? "RECORDING_SAVED" : "AUDIO_SUSPECT";
                state.detail = "quality=" + category + " source=" + provenance_;
            });
            if (g_uiRecordStop) g_uiRecordStop();
            LOGI("WAV_CLOSED session=%s reason=%s duration=%.2f packets=%llu lost=%llu peak=%d rms=%.2f quality=%s source=%s",
                 tag_.c_str(), reason.c_str(), quality_.duration(), packetCount_.load(), lost_, quality_.peak(), quality_.rms(), category.c_str(), provenance_.c_str());
        } else {
            if (diagnosticSnapshot().sessionTag == tag_) diagnosticStage(RecordingStage::Failed, reason.c_str());
            LOGW("SESSION_NO_AUDIO session=%s reason=%s invalid=%llu", tag_.c_str(), reason.c_str(), invalid_);
        }
        cleanup();
        LOGI("RTP_CLOCK_SUMMARY session=%s discontinuities=%llu gap_samples=%llu overlap_or_reset=%llu ssrc_changes=%llu",
             tag_.c_str(), timestampDiscontinuities_, timestampGapSamples_, timestampOverlapOrReset_, ssrcChanges_);
        finished_ = true;
    }

    /// Preserve the nine-column ledger contract, quoting every business-data field.
    void appendLedger() {
        std::lock_guard<std::mutex> guard(ledgerMutex_);
        const bool exists = fileExists(g_cfg.ledgerFile);
        std::ofstream output(utf8ToWide(g_cfg.ledgerFile), std::ios::binary | std::ios::app);
        if (!output) { LOGE("LEDGER_WRITE_FAILED session=%s", tag_.c_str()); return; }
        if (!exists) output << "\xEF\xBB\xBF开始时间,结束时间,时长秒,方向,本端,对端号码,对端姓名,文件名,包数\n";
        output << ledgerCell(startedAt_) << ',' << ledgerCell(nowStamp()) << ',' << quality_.duration() << ','
               << ledgerCell(direction_) << ',' << ledgerCell(localNumber_) << ',' << ledgerCell(remoteNumber_) << ','
               << ledgerCell(name_) << ',' << ledgerCell(fileName_) << ',' << packetCount_ << "\n";
        output.flush();
        if (!output) LOGE("LEDGER_WRITE_FAILED session=%s", tag_.c_str());
    }

    /// Release the adopted socket once, after the worker's last receive.
    void cleanup() { if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; } }

    /// Session identifier used only for protocol matching, never for diagnostics.
    string id_;
    /// Redacted diagnostic fingerprint.
    string tag_;
    /// Negotiated codec label for reference; actual RTP payload type selects the decoder.
    string codec_;
    /// User-owned call ledger fields, excluded from technical logs.
    string localNumber_, remoteNumber_, direction_, name_, fileName_, path_;
    /// TCP peer address allowed to supply this session's RTP.
    string controlPeer_;
    /// Source classification recorded separately from signal quality.
    string provenance_ = "unknown";
    /// Allocated UDP listening port.
    int port_ = 0;
    /// Owned UDP socket; released only by the worker/destructor.
    SOCKET socket_ = INVALID_SOCKET;
    /// Sole media worker.
    std::thread worker_;
    /// Cooperative stop request and completed-cleanup flag.
    std::atomic<bool> stop_{false}, finished_{false};
    /// Number of accepted and successfully written RTP packets.
    std::atomic<uint64_t> packetCount_{0};
    /// Valid-source lock and RTP sequence tracking.
    bool peerLocked_ = false, sequenceSeen_ = false;
    /// Last accepted RTP peer.
    sockaddr_in peer_{};
    /// Last accepted 16-bit RTP sequence number.
    int lastSequence_ = 0;
    /// Media diagnostics, modified only by the media worker.
    uint64_t lost_ = 0, duplicates_ = 0, outOfOrder_ = 0, invalid_ = 0, maxGapMs_ = 0;
    /// Last accepted media header; unsigned subtraction handles timestamp rollover.
    uint32_t lastTimestamp_ = 0, lastSsrc_ = 0, lastPayloadSize_ = 0;
    /// Observed source switches include packets rejected by the existing sequence filter.
    bool ssrcSeen_ = false;
    uint32_t observedSsrc_ = 0;
    /// Clock evidence only; this change never inserts, drops or rewrites PCM samples.
    uint64_t timestampDiscontinuities_ = 0, timestampGapSamples_ = 0, timestampOverlapOrReset_ = 0, ssrcChanges_ = 0;
    /// Wall clock at session allocation for the existing ledger.
    string startedAt_ = nowStamp();
    /// Protect the externally requested stop reason.
    std::mutex reasonMutex_;
    /// Stable completion reason; never contains remote user input.
    string reason_ = "STOPPED";
    /// Streaming PCM quality accumulator.
    PcmQuality quality_;
    /// Checked RIFF writer.
    WavWriter wav_;
    /// Serialize ledger appends from concurrent recordings.
    inline static std::mutex ledgerMutex_;
};

/// Owned TCP control service with bounded client lifetime and deterministic shutdown.
class Recorder {
public:
    /// Number of allocated sessions; this does not assert that RTP has arrived.
    std::atomic<int> activeCount{0};

    /// Stop all workers before destruction.
    ~Recorder() { shutdown(); }

    /// Bind the configured control port exclusively and start owned background loops.
    bool start() {
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) return failListen();
        BOOL exclusive = TRUE;
        setsockopt(listenSocket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(static_cast<u_short>(g_cfg.controlPort));
        if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket_, 8) != 0)
            return failListen();
        running_ = true;
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.controlListening = true; state.code = "LISTENING"; });
        LOGI("CONTROL_LISTENING port=%d advertise_ip=%s udp=%d-%d idle_frame_ms=200", g_cfg.controlPort, g_cfg.advertiseIp.c_str(), g_cfg.udpPortStart, g_cfg.udpPortEnd);
        acceptWorker_ = std::thread(&Recorder::acceptLoop, this);
        reaperWorker_ = std::thread(&Recorder::reaperLoop, this);
        return true;
    }

    /// Close the listener, wake all client sockets, and join every owned worker/session.
    void shutdown() {
        if (!running_.exchange(false)) return;
        if (listenSocket_ != INVALID_SOCKET) closesocket(listenSocket_);
        if (acceptWorker_.joinable()) acceptWorker_.join();
        listenSocket_ = INVALID_SOCKET;
        { std::lock_guard<std::mutex> guard(mutex_); for (SOCKET client : clients_) ::shutdown(client, SD_BOTH); }
        if (reaperWorker_.joinable()) reaperWorker_.join();
        for (auto& client : clientWorkers_) if (client.worker.joinable()) client.worker.join();
        clientWorkers_.clear();
        std::vector<std::shared_ptr<RecordSession>> sessions;
        { std::lock_guard<std::mutex> guard(mutex_); for (auto& entry : sessions_) sessions.push_back(entry.second); sessions_.clear(); usedPorts_.clear(); }
        for (auto& session : sessions) session->stop("SHUTDOWN");
        for (auto& session : sessions) session->join();
        activeCount = 0; g_sessionCount = 0;
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.controlListening = false; });
        LOGI("CONTROL_STOPPED workers_joined=true");
    }

    /// Cooperatively stop all recording sessions following an explicit Stop operation.
    void stopAll(const char* reason) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& entry : sessions_) entry.second->stop(reason);
    }

private:
    /// One owned client worker; completion markers let acceptLoop reap idle handles.
    struct ClientWorker {
        /// Control worker thread.
        std::thread worker;
        /// Shared marker set after socket cleanup.
        std::shared_ptr<std::atomic<bool>> done;
    };

    /// Record an observable bind failure and release the failed listener.
    bool failListen() {
        const int error = WSAGetLastError();
        if (listenSocket_ != INVALID_SOCKET) { closesocket(listenSocket_); listenSocket_ = INVALID_SOCKET; }
        diagnosticStage(RecordingStage::Failed, "CONTROL_BIND_FAILED", "port=" + std::to_string(g_cfg.controlPort) + " wsa=" + std::to_string(error));
        LOGE("CONTROL_BIND_FAILED port=%d wsa=%d", g_cfg.controlPort, error);
        return false;
    }

    /// Accept at most 16 simultaneous clients and reclaim completed thread handles.
    void acceptLoop() {
        while (running_) {
            for (auto it = clientWorkers_.begin(); it != clientWorkers_.end();) {
                if (*it->done) { it->worker.join(); it = clientWorkers_.erase(it); } else ++it;
            }
            fd_set readable; FD_ZERO(&readable); FD_SET(listenSocket_, &readable);
            timeval wait{0, 200000};
            int ready = select(0, &readable, NULL, NULL, &wait);
            if (ready <= 0) continue;
            SOCKET client = accept(listenSocket_, NULL, NULL);
            if (client == INVALID_SOCKET) continue;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (clients_.size() >= 16) { closesocket(client); LOGW("CONTROL_CLIENT_LIMIT limit=16"); continue; }
                clients_.insert(client);
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            ClientWorker worker;
            worker.done = done;
            worker.worker = std::thread([this, client, done]() {
                handleControl(client);
                { std::lock_guard<std::mutex> guard(mutex_); clients_.erase(client); }
                closesocket(client);
                *done = true;
            });
            clientWorkers_.push_back(std::move(worker));
        }
    }

    /// Read stream fragments using readiness waits; idle and EOF both flush complete frames.
    void handleControl(SOCKET client) {
        const uint64_t cid = ++nextConnection_;
        const ULONGLONG started = GetTickCount64();
        sockaddr_in address{}; int length = sizeof(address);
        getpeername(client, reinterpret_cast<sockaddr*>(&address), &length);
        const string peer = inet_ntoa(address.sin_addr);
        u_long nonblocking = 1; ioctlsocket(client, FIONBIO, &nonblocking);
        diagnosticUpdate([](DiagnosticSnapshot& state) {
            ++state.connections;
            if (state.stage == RecordingStage::Waiting || state.stage == RecordingStage::Event || state.stage == RecordingStage::Command) {
                state.attemptTcp = true; state.stage = RecordingStage::Control; state.code = "CONTROL_ACCEPTED";
            }
        });
        LOGI("CTRL_ACCEPT cid=%llu peer=%s:%u", cid, peer.c_str(), ntohs(address.sin_port));
        string buffer;
        size_t bytes = 0, frames = 0;
        ULONGLONG lastReceive = started;
        const char* closeReason = "service_stop";
        int error = 0;
        while (running_ && !g_stopping) {
            fd_set readable; FD_ZERO(&readable); FD_SET(client, &readable);
            timeval wait{0, 200000};
            const int ready = select(0, &readable, NULL, NULL, &wait);
            if (ready == SOCKET_ERROR) { error = WSAGetLastError(); closeReason = "select_error"; break; }
            if (!ready) {
                if ((bytes == 0 && GetTickCount64() - started > 30000) || (!buffer.empty() && GetTickCount64() - lastReceive > 5000)) {
                    closeReason = "incomplete_timeout"; LOGW("CTRL_INCOMPLETE_TIMEOUT cid=%llu pending=%zu", cid, buffer.size()); break;
                }
                if (!buffer.empty() && completeCandidate(buffer)) {
                    LOGD("CTRL_FRAME cid=%llu framing=idle200 bytes=%zu", cid, buffer.size());
                    if (!dispatch(buffer, client, cid, peer)) { closeReason = "reply_error"; break; }
                    ++frames; buffer.clear();
                }
                continue;
            }
            char chunk[2048];
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received == 0) {
                closeReason = "peer_fin";
                if (!buffer.empty()) {
                    LOGD("CTRL_FRAME cid=%llu framing=eof bytes=%zu complete=%d", cid, buffer.size(), completeCandidate(buffer));
                    dispatch(buffer, client, cid, peer); ++frames; buffer.clear();
                }
                break;
            }
            if (received == SOCKET_ERROR) {
                error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) continue;
                closeReason = error == WSAECONNRESET ? "peer_reset" : "recv_error"; break;
            }
            bytes += received;
            lastReceive = GetTickCount64();
            LOGD("CTRL_RX cid=%llu bytes=%d total=%zu elapsed_ms=%llu cr=%d lf=%d nul=%d", cid, received, bytes,
                 GetTickCount64() - started, memchr(chunk, '\r', received) != NULL, memchr(chunk, '\n', received) != NULL, memchr(chunk, '\0', received) != NULL);
            if (buffer.size() + received > 8192) { closeReason = "frame_limit"; LOGE("CTRL_FRAME_TOO_LARGE cid=%llu limit=8192", cid); break; }
            buffer.append(chunk, received);
            size_t delimiter;
            bool replied = true;
            while ((delimiter = firstDelimiter(buffer)) != string::npos) {
                string frame = buffer.substr(0, delimiter); buffer.erase(0, delimiter + 1);
                if (frame.empty()) continue;
                LOGD("CTRL_FRAME cid=%llu framing=delimiter bytes=%zu", cid, frame.size());
                replied = dispatch(frame, client, cid, peer); ++frames;
                if (!replied) break;
            }
            if (!replied) { closeReason = "reply_error"; break; }
        }
        logLine(error || !buffer.empty() ? "WARN" : "INFO", "CTRL_CLOSE cid=%llu cause=%s bytes=%zu frames=%zu wsa=%d elapsed_ms=%llu pending=%zu", cid, closeReason, bytes, frames, error, GetTickCount64() - started, buffer.size());
        if (frames == 0 && (bytes > 0 || diagnosticSnapshot().callActive))
            diagnosticStage(RecordingStage::Failed, error == WSAECONNRESET ? "CONTROL_RESET" : buffer.empty() ? "CONTROL_NO_PAYLOAD" : "CONTROL_INCOMPLETE");
    }

    /// Avoid idle-dispatching a fragmented Start before its required codec field arrives.
    bool completeCandidate(const string& frame) const {
        const auto values = parseKv(frame);
        const auto cmd = values.find("cmd"), id = values.find("id");
        if (cmd == values.end() || id == values.end() || id->second.empty()) return false;
        if (cmd->second == "RecordStopRequest") return true;
        const auto codec = values.find("codec");
        return cmd->second == "RecordStartRequest" && codec != values.end() && (codec->second == "PCMA" || codec->second == "PCMU");
    }

    /// Validate one complete request and check every byte of its response transmission.
    bool dispatch(const string& frame, SOCKET client, uint64_t cid, const string& peer) {
        auto params = parseKv(frame);
        const string cmd = params["cmd"], id = params["id"];
        diagnosticUpdate([](DiagnosticSnapshot& state) { ++state.frames; });
        if (id.empty() || id.size() > 96 || id.find_first_of("\r\n\0&", 0, 4) != string::npos) {
            LOGW("CTRL_BAD_ID cid=%llu frame_bytes=%zu", cid, frame.size()); return true;
        }
        const string tag = sessionFingerprint(id);
        LOGI("CTRL_COMMAND cid=%llu command=%s session=%s frame_bytes=%zu", cid,
             cmd == "RecordStartRequest" || cmd == "RecordStopRequest" ? cmd.c_str() : "unknown", tag.c_str(), frame.size());
        string reply;
        bool startAccepted = false;
        if (cmd == "RecordStartRequest") {
            int port = 0;
            string codec = params["codec"];
            const bool supported = codec == "PCMA" || codec == "PCMU";
            const bool ok = supported && startSession(id, params, port, peer);
            startAccepted = ok;
            if (!supported) diagnosticStage(RecordingStage::Failed, "UNSUPPORTED_CODEC");
            reply = "cmd=RecordStartResponse&id=" + id + (ok ? "&result=success&reason=&server=" + g_cfg.advertiseIp + "&port=" + std::to_string(port) : "&result=failed&reason=" + string(supported ? "no_port" : "unsupported_codec") + "&server=&port=0") + "\r\n";
        } else if (cmd == "RecordStopRequest") {
            stopSession(id, "PHONE_STOP");
            reply = "cmd=RecordStopResponse&id=" + id + "&result=success\r\n";
        } else { LOGW("CTRL_UNKNOWN_COMMAND cid=%llu", cid); return true; }
        const ULONGLONG started = GetTickCount64();
        size_t sentBytes = 0;
        const bool sent = sendComplete(client, reply, 2500, &sentBytes);
        const int error = sent ? 0 : WSAGetLastError();
        logLine(sent ? "INFO" : "ERROR", "CTRL_TX cid=%llu session=%s requested_bytes=%zu sent_bytes=%zu success=%d wsa=%d elapsed_ms=%llu", cid, tag.c_str(), reply.size(), sentBytes, sent, error, GetTickCount64() - started);
        if (sent) diagnosticUpdate([&](DiagnosticSnapshot& state) { ++state.responses; if (startAccepted) { state.attemptReply = true; if (!state.attemptRtp) { state.stage = RecordingStage::Reply; state.code = "CONTROL_REPLIED"; } } });
        else { diagnosticStage(RecordingStage::Failed, "CONTROL_REPLY_FAILED", "wsa=" + std::to_string(error)); stopSession(id, "CONTROL_REPLY_FAILED"); }
        return sent;
    }

    /// Allocate one exclusive UDP socket per id, reusing duplicate Start requests safely.
    bool startSession(const string& id, const std::map<string, string>& params, int& portOut, const string& peer) {
        std::lock_guard<std::mutex> admission(g_updateAdmission);
        if (g_updateInstalling || g_updateStartupPending) { LOGW("CONTROL_START_UPDATE_IN_PROGRESS"); return false; }
        g_lastCallActivity = GetTickCount64();
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = sessions_.find(id);
        if (existing != sessions_.end()) { portOut = existing->second->port(); LOGW("SESSION_DUPLICATE session=%s udp=%d", sessionFingerprint(id).c_str(), portOut); return true; }
        if (sessions_.size() >= 8) { diagnosticStage(RecordingStage::Failed, "SESSION_LIMIT"); LOGW("SESSION_LIMIT maximum=8"); return false; }
        if (!diagnosticSnapshot().callActive) diagnosticBegin("device_control");
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.attemptTcp = true; state.stage = RecordingStage::Control; state.code = "CONTROL_START_RECEIVED"; });
        for (int port = g_cfg.udpPortStart; port <= g_cfg.udpPortEnd; ++port) {
            if (usedPorts_.count(port)) continue;
            SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udp == INVALID_SOCKET) continue;
            BOOL exclusive = TRUE; setsockopt(udp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(static_cast<u_short>(port));
            if (bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(udp); continue; }
            string codec = "PCMA"; const auto value = params.find("codec"); if (value != params.end() && !value->second.empty()) codec = value->second;
            auto session = std::make_shared<RecordSession>(id, params, codec, port, udp, peer);
            sessions_[id] = session; usedPorts_.insert(port); ++activeCount; g_sessionCount = activeCount.load();
            diagnosticUpdate([&](DiagnosticSnapshot& state) { state.sessionTag = sessionFingerprint(id); });
            session->start(); portOut = port;
            LOGI("SESSION_ALLOCATED session=%s codec=%s udp=%d waiting_for_rtp=true", sessionFingerprint(id).c_str(), codec.c_str(), port);
            return true;
        }
        diagnosticStage(RecordingStage::Failed, "UDP_PORT_EXHAUSTED");
        LOGE("UDP_PORT_EXHAUSTED range=%d-%d", g_cfg.udpPortStart, g_cfg.udpPortEnd);
        return false;
    }

    /// Stop a matching session; a missing id is diagnostic, never a reason to stop another call.
    void stopSession(const string& id, const char* reason) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) found->second->stop(reason);
        else LOGW("SESSION_STOP_NOT_FOUND session=%s", sessionFingerprint(id).c_str());
    }

    /// Join completed recordings before returning their UDP ports to the pool.
    void reaperLoop() {
        while (running_) {
            Sleep(100);
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->second->stopped()) { it->second->join(); usedPorts_.erase(it->second->port()); it = sessions_.erase(it); --activeCount; g_sessionCount = activeCount.load(); }
                else ++it;
            }
        }
    }

    /// Exclusive listening socket, closed before acceptWorker_ is joined.
    SOCKET listenSocket_ = INVALID_SOCKET;
    /// Service running flag, separate from the process-wide stop request.
    std::atomic<bool> running_{false};
    /// Owned accept and session cleanup loops.
    std::thread acceptWorker_, reaperWorker_;
    /// Client workers modified only by acceptLoop and after that loop joins.
    std::vector<ClientWorker> clientWorkers_;
    /// Currently owned client sockets, used to interrupt shutdown waits.
    std::set<SOCKET> clients_;
    /// Session map and corresponding allocated ports.
    std::map<string, std::shared_ptr<RecordSession>> sessions_;
    /// Ports are released only after the owning session joins.
    std::set<int> usedPorts_;
    /// Protect sessions, ports and live clients.
    std::mutex mutex_;
    /// Monotonic per-service connection correlation number.
    std::atomic<uint64_t> nextConnection_{0};
};
