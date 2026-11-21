#pragma once

#include "device_model.h"

#include <cstring>
#include <string>

/// Fold one ASCII letter. Other bytes stay unchanged so matching does not depend on the process locale.
inline char prAsciiLower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : static_cast<char>(value);
}

/// Return an empty C string when a plugin left a pointer null.
inline const char* prText(const char* value) { return value ? value : ""; }

/// Compare two ASCII tokens without regard to case.
inline bool prEqualAscii(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        if (prAsciiLower(static_cast<unsigned char>(*left++)) != prAsciiLower(static_cast<unsigned char>(*right++))) return false;
    }
    return *left == *right;
}

/// Search for an ASCII needle without regard to case. An empty needle never matches.
inline bool prContainsAscii(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    const size_t width = std::strlen(needle);
    for (const char* cursor = haystack; *cursor; ++cursor) {
        size_t index = 0;
        while (index < width && cursor[index] &&
               prAsciiLower(static_cast<unsigned char>(cursor[index])) == prAsciiLower(static_cast<unsigned char>(needle[index]))) ++index;
        if (index == width) return true;
    }
    return false;
}

/// Report whether a case-sensitive needle occurs. Field names keep their phone spelling.
inline bool prContainsExact(const char* haystack, const char* needle) {
    return haystack && needle && *needle && std::strstr(haystack, needle) != nullptr;
}

/// Accept a device_model id. Empty is not an id; the setting uses empty for automatic matching.
inline bool deviceModelIdAllowed(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool letter = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        if (!digit && !letter && character != '.' && character != '_' && character != '-') return false;
    }
    return true;
}

/// Map one token through the component's lists. Comparison is ASCII case-insensitive.
inline int prClassifyToken(const PrDeviceProfile& profile, const char* token) {
    if (!token || !*token) return PR_EVENT_UNKNOWN;
    const auto listed = [&](const char* const* list, int kind) {
        if (!list) return 0;
        for (; *list; ++list) if (prEqualAscii(token, *list)) return kind;
        return 0;
    };
    if (const int kind = listed(profile.established_tokens, PR_EVENT_ESTABLISHED)) return kind;
    if (const int kind = listed(profile.terminated_tokens, PR_EVENT_TERMINATED)) return kind;
    if (const int kind = listed(profile.ready_tokens, PR_EVENT_READY)) return kind;
    if (const int kind = listed(profile.address_tokens, PR_EVENT_ADDRESS)) return kind;
    return PR_EVENT_UNKNOWN;
}

/// Return the token written into a newly generated callback for this event kind.
inline const char* prCanonicalToken(const PrDeviceProfile& profile, int kind) {
    const char* const* list = nullptr;
    if (kind == PR_EVENT_ESTABLISHED) list = profile.established_tokens;
    else if (kind == PR_EVENT_TERMINATED) list = profile.terminated_tokens;
    else if (kind == PR_EVENT_READY) list = profile.ready_tokens;
    else if (kind == PR_EVENT_ADDRESS) list = profile.address_tokens;
    return list && list[0] ? list[0] : "";
}

/// True when both tokens classify as the same known event for this profile.
inline bool prEventSame(const PrDeviceProfile& profile, const char* expected, const char* actual) {
    const int kind = prClassifyToken(profile, expected);
    return kind != PR_EVENT_UNKNOWN && kind == prClassifyToken(profile, actual);
}

/// Build the callback URL this component writes. Pair is omitted when empty.
inline std::string deviceEventTarget(const PrDeviceProfile& profile, const std::string& ip, int port, int kind, const std::string& pair) {
    std::string url = "http://" + ip + ":" + std::to_string(port) + prText(profile.event_path) + "?" +
        prText(profile.event_query_key) + "=" + prCanonicalToken(profile, kind);
    if (!pair.empty()) url += "&pair=" + pair;
    return url;
}

/// Build the TCP Start response using this component's command names.
inline std::string deviceControlStartReply(const PrDeviceProfile& profile, const std::string& id, bool ok, bool supported, const std::string& server, int port) {
    std::string reply = std::string("cmd=") + prText(profile.ctrl_start_response) + "&id=" + id;
    if (ok) reply += "&result=success&reason=&server=" + server + "&port=" + std::to_string(port);
    else reply += "&result=failed&reason=" + std::string(supported ? "no_port" : "unsupported_codec") + "&server=&port=0";
    return reply + "\r\n";
}

/// Build the TCP Stop response using this component's command name.
inline std::string deviceControlStopReply(const PrDeviceProfile& profile, const std::string& id) {
    return std::string("cmd=") + prText(profile.ctrl_stop_response) + "&id=" + id + "&result=success\r\n";
}

/// True when a non-empty protocol token is exactly the received command.
inline bool deviceCommandIs(const char* expected, const std::string& command) {
    return expected && *expected && command == expected;
}
