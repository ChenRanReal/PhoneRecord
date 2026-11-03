#pragma once

/// Append a validated Unicode scalar as UTF-8, including supplementary characters.
static void appendJsonScalar(string& output, uint32_t value) {
    if (value <= 0x7f) output += static_cast<char>(value);
    else if (value <= 0x7ff) { output += static_cast<char>(0xc0 | (value >> 6)); output += static_cast<char>(0x80 | (value & 63)); }
    else if (value <= 0xffff) { output += static_cast<char>(0xe0 | (value >> 12)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
    else { output += static_cast<char>(0xf0 | (value >> 18)); output += static_cast<char>(0x80 | ((value >> 12) & 63)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
}

/// Read exactly four JSON hexadecimal digits without accepting malformed escape sequences.
static bool readJsonHex(const string& input, size_t& position, uint32_t& value) {
    value = 0;
    for (int digit = 0; digit < 4; ++digit) {
        if (position >= input.size()) return false;
        const unsigned char byte = input[position++];
        const int part = byte >= '0' && byte <= '9' ? byte - '0' : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : byte >= 'A' && byte <= 'F' ? byte - 'A' + 10 : -1;
        if (part < 0) return false;
        value = (value << 4) | part;
    }
    return true;
}

/// Decode one JSON string including escaped quotes, Unicode and surrogate pairs.
static bool readJsonString(const string& input, size_t& position, string& output) {
    output.clear();
    if (position >= input.size() || input[position++] != '"') return false;
    while (position < input.size()) {
        const unsigned char byte = input[position++];
        if (byte == '"') return true;
        if (byte < 0x20) return false;
        if (byte != '\\') { output += static_cast<char>(byte); continue; }
        if (position >= input.size()) return false;
        const char escape = input[position++];
        if (escape == '"' || escape == '\\' || escape == '/') output += escape;
        else if (escape == 'b') output += '\b';
        else if (escape == 'f') output += '\f';
        else if (escape == 'n') output += '\n';
        else if (escape == 'r') output += '\r';
        else if (escape == 't') output += '\t';
        else if (escape == 'u') {
            uint32_t scalar = 0;
            if (!readJsonHex(input, position, scalar)) return false;
            if (scalar >= 0xd800 && scalar <= 0xdbff) {
                if (input.compare(position, 2, "\\u") != 0) return false;
                position += 2; uint32_t low = 0;
                if (!readJsonHex(input, position, low) || low < 0xdc00 || low > 0xdfff) return false;
                scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
            } else if (scalar >= 0xdc00 && scalar <= 0xdfff) return false;
            appendJsonScalar(output, scalar);
        } else return false;
        if (output.size() > 65536) return false;
    }
    return false;
}

/// Find a string-valued metadata property without exposing JSON escape text to the operator.
static string jsonField(const string& body, const string& key) {
    size_t position = 0;
    while ((position = body.find('"', position)) != string::npos) {
        string field;
        if (!readJsonString(body, position, field)) return "";
        size_t value = body.find_first_not_of(" \t\r\n", position);
        if (field != key || value == string::npos || body[value] != ':') continue;
        value = body.find_first_not_of(" \t\r\n", value + 1);
        string result;
        return value != string::npos && readJsonString(body, value, result) ? result : "";
    }
    return "";
}

/// Quote business metadata as JSON while keeping it out of technical diagnostic events.
static string jsonQuoted(const string& value) {
    string output = "\"";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') { output += '\\'; output += static_cast<char>(byte); }
        else if (byte < 0x20) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", byte); output += escaped; }
        else output += static_cast<char>(byte);
    }
    return output + '"';
}
