#pragma once

/// Normalize a unicast Ethernet identity; separators are optional, malformed input stays invalid.
static string normalizePhoneMac(const string& value) {
    string result;
    for (unsigned char ch : value) {
        if (ch == ':' || ch == '-') continue;
        if (!isxdigit(ch)) return "";
        result += static_cast<char>(tolower(ch));
    }
    if (result.size() != 12 || result == "000000000000" || (strtoul(result.substr(0, 2).c_str(), nullptr, 16) & 1)) return "";
    return result;
}
