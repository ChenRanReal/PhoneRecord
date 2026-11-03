#pragma once

/// Checked 8 kHz/16-bit/mono RIFF writer; failures never become silent success.
class WavWriter {
public:
    /// Finalize an open file even when the owning session exits early.
    ~WavWriter() { close(); }

    /// Create a new recording without overwriting an existing call file.
    bool open(const string& path) {
        file_ = CreateFileW(utf8ToWide(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); LOGE("WAV_OPEN_FAILED win32=%lu", error_); return false; }
        const uint8_t header[44] = {
            'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,'d','a','t','a',0,0,0,0
        };
        if (!writeExact(header, sizeof(header))) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; return false; }
        return true;
    }

    /// Write complete PCM samples, checking disk errors and the RIFF 32-bit size limit.
    bool write(const int16_t* samples, int count) {
        if (file_ == INVALID_HANDLE_VALUE || count <= 0) return false;
        if (dataBytes_ + static_cast<uint64_t>(count) * 2 > 0xffffffffULL - 36) { error_ = ERROR_FILE_TOO_LARGE; return false; }
        const bool ok = writeExact(samples, static_cast<DWORD>(count * 2));
        if (ok) dataBytes_ += static_cast<uint64_t>(count) * 2;
        return ok;
    }

    /// Patch both RIFF lengths, flush, and always close the native file handle.
    bool close() {
        if (file_ == INVALID_HANDLE_VALUE) return error_ == ERROR_SUCCESS;
        LARGE_INTEGER position{}; position.QuadPart = 4;
        const uint32_t riffSize = static_cast<uint32_t>(dataBytes_ + 36);
        const uint32_t dataSize = static_cast<uint32_t>(dataBytes_);
        bool ok = SetFilePointerEx(file_, position, NULL, FILE_BEGIN) != FALSE && writeExact(&riffSize, 4);
        position.QuadPart = 40;
        ok = (SetFilePointerEx(file_, position, NULL, FILE_BEGIN) != FALSE && writeExact(&dataSize, 4)) && ok;
        if (!FlushFileBuffers(file_)) { error_ = GetLastError(); ok = false; }
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        if (!ok) LOGE("WAV_FINALIZE_FAILED win32=%lu", error_);
        return ok && error_ == ERROR_SUCCESS;
    }

    /// Return only bytes whose complete write succeeded.
    uint64_t dataBytes() const { return dataBytes_; }
    /// Return whether a file is currently open.
    bool isOpen() const { return file_ != INVALID_HANDLE_VALUE; }

private:
    /// Handle partial WriteFile results and retain the original system error.
    bool writeExact(const void* data, DWORD bytes) {
        DWORD offset = 0;
        while (offset < bytes) {
            DWORD written = 0;
            if (!WriteFile(file_, static_cast<const BYTE*>(data) + offset, bytes - offset, &written, NULL) || written == 0) {
                error_ = GetLastError(); if (!error_) error_ = ERROR_WRITE_FAULT;
                LOGE("WAV_WRITE_FAILED win32=%lu requested=%lu completed=%lu", error_, bytes, offset);
                return false;
            }
            offset += written;
        }
        return true;
    }

    /// Sole owned native file handle.
    HANDLE file_ = INVALID_HANDLE_VALUE;
    /// Successfully written audio bytes, excluding the RIFF header.
    uint64_t dataBytes_ = 0;
    /// First retained storage failure, reported separately from audio quality.
    DWORD error_ = ERROR_SUCCESS;
};
