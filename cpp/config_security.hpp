#pragma once

#pragma comment(lib, "crypt32.lib")

/// Encrypt a local saved credential for the current Windows account; never log it.
static bool protectLocalSecret(const std::string& plain, std::string& encoded) {
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"PhoneRecord", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    DWORD length = 0;
    bool ok = CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &length) != FALSE;
    std::string value(length, '\0');
    if (ok) ok = CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &value[0], &length) != FALSE;
    LocalFree(output.pbData);
    if (!ok) return false;
    if (!value.empty() && value.back() == '\0') value.pop_back();
    encoded = "dpapi:" + value;
    return true;
}

/// Decrypt a current-account credential, accepting legacy plaintext only when unprefixed.
static bool unprotectLocalSecret(const std::string& encoded, std::string& plain) {
    if (encoded.compare(0, 6, "dpapi:") != 0) { plain = encoded; return true; }
    const std::string value = encoded.substr(6);
    DWORD length = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, NULL, &length, NULL, NULL)) return false;
    std::vector<BYTE> data(length);
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, data.data(), &length, NULL, NULL)) return false;
    DATA_BLOB input{length, data.data()}, output{};
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    plain.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

/// Validate the IPv4-only address contract used by this phone and the native sockets.
static bool validIpv4(const std::string& value) {
    IN_ADDR address{};
    return InetPtonA(AF_INET, value.c_str(), &address) == 1 &&
           address.s_addr != INADDR_ANY && address.s_addr != INADDR_BROADCAST;
}

/// Parse a complete decimal configuration value; malformed values remain visibly invalid.
static int configurationInteger(const std::string& value) {
    if (value.empty() || value.size() > 6) return -1;
    for (char byte : value) if (byte < '0' || byte > '9') return -1;
    return atoi(value.c_str());
}
