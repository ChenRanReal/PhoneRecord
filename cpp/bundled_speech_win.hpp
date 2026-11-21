#pragma once

#include "offline_rules.hpp"

#include <windows.h>

#include <map>
#include <string>

/// Convert a forward-slash bundle path to a Windows relative path. Pins are ASCII.
inline std::wstring speechRelativeWide(const char* relative) {
    std::wstring path;
    if (!relative) return path;
    for (const char* cursor = relative; *cursor; ++cursor) path += *cursor == '/' ? L'\\' : static_cast<wchar_t>(*cursor);
    return path;
}

/// Record size and presence for every pin and runtime file. Digests are left empty.
inline void speechStatBundle(const std::wstring& directory, std::map<std::string, SpeechFileFact>& files) {
    auto consider = [&](const char* relative) {
        SpeechFileFact fact;
        const std::wstring path = directory + L"\\" + speechRelativeWide(relative);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            fact.present = true;
            fact.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
        files[relative] = fact;
    };
    for (const SpeechPin& pin : kSpeechPins) consider(pin.relative);
    for (const SpeechRuntimeFile& file : kSpeechRuntimeFiles) consider(file.relative);
}
