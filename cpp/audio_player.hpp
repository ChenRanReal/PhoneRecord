#pragma once

/// Owned streaming WAV playback using WinMM directly, without a legacy MCI driver or shell player.
class WavPlayer {
public:
    /// Release playback buffers and the output device before the view is destroyed.
    ~WavPlayer() { close(); }

    /// Validate a complete phone PCM WAV and adopt one independent waveform output device.
    bool open(const string& path, HWND) {
        close();
        errorKey_ = "message.player_failed"; errorOperation_.clear(); errorResult_ = MMSYSERR_NOERROR;
        input_.open(utf8ToWide(path), std::ios::binary);
        if (!input_.is_open()) { fail("message.player_file", "file"); close(); return false; }
        if (!readFormat()) { fail("message.player_invalid", "format"); close(); return false; }
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1;
        format.nSamplesPerSec = 8000; format.nAvgBytesPerSec = 16000; format.nBlockAlign = 2; format.wBitsPerSample = 16;
        MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (result == MMSYSERR_BADDEVICEID) {
            // A stale/missing default mapping can coexist with working endpoints on a new PC.
            // Retry only this observed error, with a bounded scan of currently enumerated devices.
            const UINT devices = std::min<UINT>(waveOutGetNumDevs(), 32);
            for (UINT id = 0; id < devices; ++id) {
                device_ = NULL;
                const MMRESULT candidate = waveOutOpen(&device_, id, &format, 0, 0, CALLBACK_NULL);
                LOGI("PLAYBACK_DEVICE_ATTEMPT device_id=%u mm_result=%u mapper_mm_result=%u", id, candidate, MMSYSERR_BADDEVICEID);
                if (candidate == MMSYSERR_NOERROR) { result = candidate; LOGI("PLAYBACK_DEVICE_FALLBACK device_id=%u mapper_mm_result=%u", id, MMSYSERR_BADDEVICEID); break; }
                if (candidate != MMSYSERR_BADDEVICEID && candidate != MMSYSERR_NODRIVER) result = candidate;
            }
        }
        if (!success(result, "open")) { device_ = NULL; close(); return false; }
        if (!success(waveOutPause(device_), "pause")) { close(); return false; }
        for (auto& buffer : buffers_) {
            buffer.header = {}; buffer.header.lpData = buffer.data; buffer.header.dwBufferLength = sizeof(buffer.data);
            if (!success(waveOutPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)), "prepare")) { close(); return false; }
        }
        path_ = path; nextByte_ = baseByte_ = 0;
        input_.clear(); input_.seekg(dataOffset_);
        LOGI("PLAYBACK_OPENED duration_ms=%lu internal=true backend=waveout output_rate=%lu output_channels=%u", length(), format.nSamplesPerSec, format.nChannels);
        return true;
    }

    /// Resume queued samples, or restart from zero after natural completion.
    bool play() {
        if (!device_) return false;
        if (ended_ && !seek(0)) return false;
        playing_ = true; ended_ = false;
        if (!pump()) return false;
        const bool ok = success(waveOutRestart(device_), "play");
        if (!ok) playing_ = false;
        else LOGI("PLAYBACK_PLAY internal=true backend=waveout");
        return ok;
    }

    /// Pause the device while retaining both the cursor and queued samples.
    bool pause() {
        if (!device_ || !playing_) return false;
        const bool ok = success(waveOutPause(device_), "pause");
        if (ok) { playing_ = false; LOGI("PLAYBACK_PAUSED"); }
        return ok;
    }

    /// Stop and rewind the loaded recording without changing any other audio session.
    bool stop() { if (!device_) return false; const bool ok = seek(0); if (ok) LOGI("PLAYBACK_STOPPED"); return ok; }

    /// Reset returned buffers, reposition on a sample boundary and optionally continue playback.
    bool seek(DWORD milliseconds, bool preservePlayback = false) {
        if (!device_) return false;
        const bool resume = preservePlayback && playing_;
        if (!success(waveOutReset(device_), "reset")) return false;
        if (!success(waveOutPause(device_), "pause")) return false;
        for (auto& buffer : buffers_) buffer.queued = false;
        baseByte_ = nextByte_ = static_cast<DWORD>(std::min<uint64_t>(dataBytes_, static_cast<uint64_t>(milliseconds) * 16)) & ~1UL;
        input_.clear(); input_.seekg(dataOffset_ + baseByte_);
        playing_ = false; ended_ = baseByte_ == dataBytes_;
        LOGI("PLAYBACK_SEEK position_ms=%lu", baseByte_ / 16);
        return !resume || ended_ || play();
    }

    /// Recycle only returned buffers; bounded streaming never loads an entire long call into RAM.
    bool pump() {
        if (!device_ || !playing_) return true;
        bool pending = false;
        for (auto& buffer : buffers_) {
            if (buffer.queued && (buffer.header.dwFlags & WHDR_DONE)) buffer.queued = false;
            if (!buffer.queued && nextByte_ < dataBytes_) {
                const DWORD count = std::min<DWORD>(sizeof(buffer.data), dataBytes_ - nextByte_);
                if (!input_.read(buffer.data, count)) { fail("message.player_invalid", "read"); waveOutReset(device_); playing_ = false; return false; }
                buffer.header.dwBufferLength = count;
                if (!success(waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)), "write")) { waveOutReset(device_); playing_ = false; return false; }
                buffer.queued = true; nextByte_ += count;
            }
            pending = pending || buffer.queued;
        }
        if (!pending && nextByte_ == dataBytes_) { playing_ = false; ended_ = true; LOGI("PLAYBACK_ENDED"); }
        return true;
    }

    /// Stop output before unpreparing stable headers and releasing their backing storage.
    void close() {
        if (device_) {
            success(waveOutReset(device_), "reset_close");
            for (auto& buffer : buffers_) { if (buffer.header.dwFlags & WHDR_PREPARED) success(waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR)), "unprepare"); buffer.queued = false; }
            success(waveOutClose(device_), "close"); device_ = NULL;
        }
        input_.close(); input_.clear(); playing_ = ended_ = false; dataBytes_ = nextByte_ = baseByte_ = 0; path_.clear();
    }

    /// Read the hardware cursor in its returned unit, relative to the most recent reset.
    DWORD position() const {
        if (!device_) return 0;
        if (ended_) return length();
        MMTIME time{}; time.wType = TIME_BYTES;
        uint64_t bytes = baseByte_;
        if (waveOutGetPosition(device_, &time, sizeof(time)) == MMSYSERR_NOERROR) {
            if (time.wType == TIME_BYTES) bytes += time.u.cb;
            else if (time.wType == TIME_SAMPLES) bytes += static_cast<uint64_t>(time.u.sample) * 2;
            else if (time.wType == TIME_MS) bytes += static_cast<uint64_t>(time.u.ms) * 16;
        }
        return static_cast<DWORD>(std::min<uint64_t>(bytes, dataBytes_) / 16);
    }
    /// Return duration from the validated PCM data chunk rather than overall file size.
    DWORD length() const { return dataBytes_ / 16; }
    /// Return whether a validated recording and output device are loaded.
    bool loaded() const { return device_ != NULL; }
    /// Return whether the device should currently transmit queued audio.
    bool playing() const { return playing_; }
    /// Return the loaded path to preserve pause/resume when Play is pressed again.
    const string& path() const { return path_; }

    /// Return a stable resource key for the last failure; cleanup does not overwrite its cause.
    const char* errorKey() const { return errorKey_; }
    /// Return private-data-free technical details for a localized operator error dialog.
    wstring errorDetails() const { return L"PLAYBACK_FAILED operation=" + utf8ToWide(errorOperation_) + L" mm_result=" + std::to_wstring(errorResult_); }

private:
    /// Validate chunk boundaries against the actual file and accept the recorder's PCM contract.
    bool readFormat() {
        input_.seekg(0, std::ios::end); const std::streamoff fileSize = input_.tellg(); input_.seekg(0);
        char riff[12];
        if (fileSize < 44 || !input_.read(riff, 12) || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) return false;
        bool pcm = false, data = false;
        for (int chunks = 0; chunks < 256 && input_; ++chunks) {
            char name[4]; uint32_t size = 0;
            if (!input_.read(name, 4) || !input_.read(reinterpret_cast<char*>(&size), 4)) break;
            const std::streamoff offset = input_.tellg();
            if (offset < 0 || static_cast<uint64_t>(offset) + size > static_cast<uint64_t>(fileSize)) return false;
            if (!memcmp(name, "fmt ", 4)) {
                if (size < 16) return false;
                uint8_t format[16]; if (!input_.read(reinterpret_cast<char*>(format), 16)) return false;
                uint32_t rate = 0; memcpy(&rate, format + 4, 4);
                pcm = format[0] == 1 && format[1] == 0 && format[2] == 1 && format[3] == 0 && rate == 8000 && format[12] == 2 && format[13] == 0 && format[14] == 16 && format[15] == 0;
            } else if (!memcmp(name, "data", 4)) { dataOffset_ = offset; dataBytes_ = size; data = size > 0 && (size & 1) == 0; }
            if (pcm && data) return true;
            input_.seekg(offset + static_cast<std::streamoff>(size) + (size & 1));
        }
        return false;
    }

    /// Retain the actionable cause without exposing caller details or file paths.
    void fail(const char* key, const char* operation, MMRESULT result = MMSYSERR_NOERROR) {
        errorKey_ = key; errorOperation_ = operation; errorResult_ = result;
        LOGW("PLAYBACK_FAILED operation=%s mm_result=%u output_devices=%u reason=%s", operation, result, waveOutGetNumDevs(), key);
    }
    /// Classify device errors; cleanup failures are logged without hiding the original failure.
    bool success(MMRESULT result, const char* operation) {
        if (result == MMSYSERR_NOERROR) return true;
        if (!strcmp(operation, "reset_close") || !strcmp(operation, "unprepare") || !strcmp(operation, "close")) {
            LOGW("PLAYBACK_CLEANUP_FAILED operation=%s mm_result=%u", operation, result); return false;
        }
        const char* key = "message.player_device";
        if (result == MMSYSERR_NODRIVER || result == MMSYSERR_BADDEVICEID || (result == MMSYSERR_ERROR && waveOutGetNumDevs() == 0)) key = "message.player_no_device";
        else if (result == MMSYSERR_ALLOCATED) key = "message.player_busy";
        else if (result == WAVERR_BADFORMAT) key = "message.player_format";
        fail(key, operation, result); return false;
    }
    /// Stable per-device buffer/header storage, prepared once and reused after WHDR_DONE.
    struct Buffer { WAVEHDR header{}; char data[4096]{}; bool queued = false; } buffers_[4];
    /// Last actionable error survives close() so the UI can explain failed open attempts.
    const char* errorKey_ = "message.player_failed";
    string errorOperation_;
    MMRESULT errorResult_ = MMSYSERR_NOERROR;
    /// Owned system waveform handle and input stream, used only on the GUI thread.
    HWAVEOUT device_ = NULL;
    std::ifstream input_;
    /// PCM chunk position, current reset base and next not-yet-queued sample byte.
    std::streamoff dataOffset_ = 0;
    DWORD dataBytes_ = 0, nextByte_ = 0, baseByte_ = 0;
    /// Loaded path and independent playback state.
    string path_;
    bool playing_ = false, ended_ = false;
};
