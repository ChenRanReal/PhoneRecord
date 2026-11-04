#pragma once

/// A bounded per-recording title snapshot; original call metadata is never rewritten.
struct RecordingTitle {
    bool exists = false;
    DWORD error = ERROR_SUCCESS;
    wstring value;
    string bytes;
};

/// Result codes keep business titles out of technical diagnostic messages.
enum class TitleResult { Saved, Invalid, Busy, Changed, Metadata, Storage };

/// Trim Unicode whitespace and validate at most 120 Unicode scalars on one line.
static bool normalizeRecordingTitle(const wstring& input, wstring& output) {
    const auto space = [](wchar_t value) { WORD type = 0; return GetStringTypeW(CT_CTYPE1, &value, 1, &type) && (type & C1_SPACE); };
    size_t begin = 0, end = input.size();
    while (begin < end && space(input[begin])) ++begin;
    while (end > begin && space(input[end - 1])) --end;
    output = input.substr(begin, end - begin);
    size_t characters = 0;
    for (size_t index = 0; index < output.size(); ++index) {
        const wchar_t value = output[index];
        if (value < 0x20 || (value >= 0x7f && value <= 0x9f) || value == 0x2028 || value == 0x2029 || (value >= 0xdc00 && value <= 0xdfff)) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++index >= output.size() || output[index] < 0xdc00 || output[index] > 0xdfff) return false;
        }
        if (++characters > 120) return false;
    }
    return true;
}

/// Read one strict UTF-8 title object without treating broken metadata as an empty title.
static RecordingTitle readRecordingTitle(const wstring& recording) {
    RecordingTitle result;
    HANDLE file = CreateFileW((recording + L".title.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) result.error = error;
        return result;
    }
    result.exists = true;
    LARGE_INTEGER size{}; DWORD read = 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 2 || size.QuadPart > 4096) {
        CloseHandle(file); result.error = ERROR_INVALID_DATA; return result;
    }
    result.bytes.resize(static_cast<size_t>(size.QuadPart));
    const BOOL readOk = ReadFile(file, &result.bytes[0], static_cast<DWORD>(result.bytes.size()), &read, NULL);
    const bool loaded = readOk && read == result.bytes.size();
    const DWORD error = readOk ? loaded ? ERROR_SUCCESS : ERROR_READ_FAULT : GetLastError(); CloseHandle(file);
    if (!loaded) { result.error = error ? error : ERROR_READ_FAULT; return result; }
    size_t position = 0;
    const auto skip = [&]() { const size_t next = result.bytes.find_first_not_of(" \t\r\n", position); position = next == string::npos ? result.bytes.size() : next; };
    skip(); bool valid = position < result.bytes.size() && result.bytes[position++] == '{'; skip();
    string key, value;
    valid = valid && readJsonString(result.bytes, position, key) && key == "title"; skip();
    valid = valid && position < result.bytes.size() && result.bytes[position++] == ':'; skip();
    valid = valid && readJsonString(result.bytes, position, value); skip();
    valid = valid && position < result.bytes.size() && result.bytes[position++] == '}'; skip();
    valid = valid && position == result.bytes.size();
    const int count = value.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), NULL, 0);
    if (!value.empty() && count <= 0) valid = false;
    wstring title(static_cast<size_t>(std::max(0, count)), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), &title[0], count);
    if (!valid || !normalizeRecordingTitle(title, result.value)) result.error = ERROR_INVALID_DATA;
    return result;
}

/// Commit only a title sidecar, rejecting active writers, stale dialogs and invalid existing data.
static TitleResult saveRecordingTitle(const wstring& recording, const wstring& input, const RecordingTitle& expected, DWORD& error) {
    error = ERROR_SUCCESS;
    wstring title;
    if (!normalizeRecordingTitle(input, title)) return TitleResult::Invalid;
    HANDLE file = CreateFileW(recording.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? TitleResult::Busy : TitleResult::Storage;
    }
    // Keep the recording read handle open until commit so a writer cannot start midway.
    const auto finish = [&](TitleResult result, DWORD failure = ERROR_SUCCESS) { error = failure; CloseHandle(file); return result; };
    const RecordingTitle current = readRecordingTitle(recording);
    if (current.error) return finish(current.error == ERROR_INVALID_DATA ? TitleResult::Metadata : TitleResult::Storage, current.error);
    if (current.exists != expected.exists || current.bytes != expected.bytes) return finish(TitleResult::Changed);
    const wstring destination = recording + L".title.json";
    if (title.empty()) {
        if (current.exists && !DeleteFileW(destination.c_str())) return finish(TitleResult::Storage, GetLastError());
        return finish(TitleResult::Saved);
    }
    const wstring pending = destination + L".pending-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE output = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return finish(TitleResult::Storage, GetLastError());
    const string bytes = "{\"title\":" + jsonQuoted(wideToUtf8(title)) + "}\n";
    DWORD written = 0;
    const bool saved = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, NULL) && written == bytes.size() && FlushFileBuffers(output);
    DWORD failure = saved ? ERROR_SUCCESS : GetLastError(); CloseHandle(output);
    if (saved && MoveFileExW(pending.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return finish(TitleResult::Saved);
    if (saved) failure = GetLastError();
    DeleteFileW(pending.c_str());
    return finish(TitleResult::Storage, failure ? failure : ERROR_WRITE_FAULT);
}
