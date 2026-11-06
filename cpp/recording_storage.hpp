#pragma once

/// Capture local calendar time once, so a call spanning midnight retains its initial folder.
static SYSTEMTIME recordingLocalTime() {
    SYSTEMTIME value{}; GetLocalTime(&value); return value;
}

/// Format a local calendar date without locale-specific separators.
static string recordingDate(const SYSTEMTIME& value) {
    char text[16]{}; snprintf(text, sizeof(text), "%04u-%02u-%02u", value.wYear, value.wMonth, value.wDay); return text;
}

/// Use the same captured start instant for folder, filename and ledger timestamp.
static string recordingTimestamp(const SYSTEMTIME& value, bool compact = false) {
    char text[32]{};
    snprintf(text, sizeof(text), compact ? "%04u%02u%02u-%02u%02u%02u" : "%04u-%02u-%02u %02u:%02u:%02u",
             value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond);
    return text;
}

/// Return the Monday date for a local calendar day, including month/year boundaries.
static string recordingWeekStart(SYSTEMTIME value) {
    value.wHour = value.wMinute = value.wSecond = value.wMilliseconds = 0;
    FILETIME file{}; if (!SystemTimeToFileTime(&value, &file)) return "";
    ULARGE_INTEGER ticks{}; ticks.LowPart = file.dwLowDateTime; ticks.HighPart = file.dwHighDateTime;
    ticks.QuadPart -= static_cast<ULONGLONG>((value.wDayOfWeek + 6) % 7) * 864000000000ULL;
    file.dwLowDateTime = ticks.LowPart; file.dwHighDateTime = ticks.HighPart;
    if (!FileTimeToSystemTime(&file, &value)) return "";
    return recordingDate(value);
}

/// Accept only bounded relative directory components; reject Windows device names and traversal.
static bool validRecordingSubfolder(const string& relative) {
    if (relative.empty() || relative.size() > 180) return false;
    size_t start = 0; int depth = 0;
    while (start <= relative.size()) {
        const size_t end = relative.find_first_of("/\\", start);
        const string part = relative.substr(start, end == string::npos ? string::npos : end - start);
        if (++depth > 8 || part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ' || part.back() == '.') return false;
        for (unsigned char byte : part) if (byte < 32 || byte == 127 || string("<>:\"|?*{}").find(static_cast<char>(byte)) != string::npos) return false;
        const string name = toLower(part.substr(0, part.find('.')));
        if (name == "con" || name == "prn" || name == "aux" || name == "nul" ||
            (name.size() == 4 && (name.substr(0, 3) == "com" || name.substr(0, 3) == "lpt") && name[3] >= '1' && name[3] <= '9')) return false;
        if (end == string::npos) break;
        start = end + 1;
    }
    return true;
}

/// Expand the documented calendar tokens; unknown tokens remain invalid rather than silently changing meaning.
static bool recordingSubfolder(const string& mode, const string& pattern, const SYSTEMTIME& time, string& result) {
    result.clear();
    if (mode == "flat") return true;
    if (mode == "daily") result = recordingDate(time);
    else if (mode == "weekly") result = recordingWeekStart(time);
    else if (mode == "custom") {
        if (pattern.empty() || pattern.size() > 120) return false;
        const string date = recordingDate(time);
        const std::pair<string, string> tokens[] = {{"{yyyy}", date.substr(0, 4)}, {"{MM}", date.substr(5, 2)}, {"{dd}", date.substr(8, 2)}, {"{weekStart}", recordingWeekStart(time)}};
        result = pattern;
        for (const auto& token : tokens) {
            size_t position = 0;
            while ((position = result.find(token.first, position)) != string::npos) { result.replace(position, token.first.size(), token.second); position += token.second.size(); }
        }
    } else return false;
    std::replace(result.begin(), result.end(), '/', '\\');
    return validRecordingSubfolder(result);
}

/// Create child folders below the chosen root without following a nested junction or symbolic link.
static bool ensureRecordingSubfolder(const string& root, const string& relative) {
    if (!ensureDir(root)) return false;
    if (relative.empty()) return true;
    if (!validRecordingSubfolder(relative)) { SetLastError(ERROR_INVALID_NAME); return false; }
    string path = root; size_t start = 0;
    while (start < relative.size()) {
        const size_t end = relative.find('\\', start);
        path += "\\" + relative.substr(start, end == string::npos ? string::npos : end - start);
        if (!CreateDirectoryW(utf8ToWide(path).c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const DWORD attributes = GetFileAttributesW(utf8ToWide(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { SetLastError(ERROR_ACCESS_DENIED); return false; }
        if (end == string::npos) break;
        start = end + 1;
    }
    return true;
}

/// One discovered file keeps its path relative to the configured root, including legacy flat files.
struct RecordingFile {
    /// Stable relative identity used by the ledger, sidecars, playback and rename.
    string relative;
    /// File length captured by the same directory enumeration.
    long long size = 0;
};

/// Enumerate bounded subdirectories without traversing junctions, preserving old and new folder schemes.
static std::vector<RecordingFile> recordingFiles(const string& root) {
    std::vector<RecordingFile> files;
    std::vector<std::pair<string, int>> pending{{"", 0}};
    size_t visited = 0;
    while (!pending.empty() && visited++ < 100000) {
        const auto folder = pending.back(); pending.pop_back();
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(utf8ToWide(root + "\\" + folder.first + "*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            const wstring name = data.cFileName;
            if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const string relative = folder.first + wideToUtf8(name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (folder.second < 8) pending.push_back({relative + "\\", folder.second + 1});
            } else if (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".wav") == 0)
                files.push_back({relative, (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow});
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    return files;
}
