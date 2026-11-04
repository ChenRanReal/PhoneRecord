#pragma once

#include <wincrypt.h>
#include <array>
#include "product_version.h"
#include "update_public_key.hpp"

#ifdef PHONERECORD_UPDATE_TESTING
/// Ephemeral test trust root, compiled only into the native test executable.
static string g_updateTestKey;
#endif

/// Return the compiled publisher trust root; production has no runtime key override.
static const char* updatePinnedKey() {
#ifdef PHONERECORD_UPDATE_TESTING
    if (!g_updateTestKey.empty()) return g_updateTestKey.c_str();
#endif
    return kUpdatePublicKey;
}

/// Three bounded numeric components; no preview suffixes or ambiguous leading zeroes.
static bool updateVersion(const string& text, std::array<unsigned, 3>& parts) {
    size_t start = 0;
    for (size_t index = 0; index < parts.size(); ++index) {
        const size_t end = text.find('.', start);
        const string part = text.substr(start, end == string::npos ? end : end - start);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part[0] == '0')) return false;
        unsigned value = 0;
        for (char ch : part) { if (ch < '0' || ch > '9') return false; value = value * 10 + ch - '0'; }
        if (value > 65535 || (index < 2) != (end != string::npos)) return false;
        parts[index] = value; start = end + 1;
    }
    return true;
}

/// Compare parsed versions numerically; malformed candidates are never installable.
static bool updateNewer(const string& candidate, const string& current = PHONERECORD_VERSION) {
    std::array<unsigned, 3> left{}, right{};
    return updateVersion(candidate, left) && updateVersion(current, right) && left > right;
}

/// Decode bounded standard base64 without accepting trailing garbage.
static bool updateUnbase64(const string& text, std::vector<BYTE>& bytes) {
    if (text.empty() || text.size() > 8192) return false;
    DWORD length = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, NULL, &length, NULL, NULL)) return false;
    bytes.resize(length);
    return CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, bytes.data(), &length, NULL, NULL) != FALSE;
}

/// Verify exact manifest bytes with the pinned RSA key and SHA-256/PKCS#1 v1.5.
static bool updateSignature(const string& manifest, const string& signature, const char* publicKey = updatePinnedKey()) {
    std::vector<BYTE> keyBytes, signedBytes;
    if (manifest.empty() || manifest.size() > 4096 || !updateUnbase64(publicKey, keyBytes) || !updateUnbase64(signature, signedBytes)) return false;
    HCRYPTPROV provider = 0; HCRYPTKEY key = 0; HCRYPTHASH hash = 0;
    bool valid = false;
    if (CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptImportKey(provider, keyBytes.data(), static_cast<DWORD>(keyBytes.size()), 0, 0, &key) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(manifest.data()), static_cast<DWORD>(manifest.size()), 0)) {
        // .NET/OpenSSL emit big-endian RSA signatures; CryptoAPI expects little-endian.
        std::reverse(signedBytes.begin(), signedBytes.end());
        valid = CryptVerifySignatureW(hash, signedBytes.data(), static_cast<DWORD>(signedBytes.size()), key, NULL, 0) != FALSE;
    }
    if (hash) CryptDestroyHash(hash);
    if (key) CryptDestroyKey(key);
    if (provider) CryptReleaseContext(provider, 0);
    return valid;
}

/// Signed release metadata; only a fixed executable name and architecture are accepted.
struct UpdateManifest {
    /// Stable semantic version authenticated by the signature.
    string version;
    /// Lowercase SHA-256 authenticated by the signature.
    string sha256;
    /// Exact signed executable byte count, capped at 64 MiB.
    uint64_t size = 0;

    /// Parse the canonical LF-only six-line format, rejecting unknown/duplicate fields.
    bool parse(const string& bytes) {
        std::istringstream input(bytes); string magic, versionLine, architecture, file, sizeLine, hashLine, extra;
        if (!std::getline(input, magic) || !std::getline(input, versionLine) || !std::getline(input, architecture) ||
            !std::getline(input, file) || !std::getline(input, sizeLine) || !std::getline(input, hashLine) || std::getline(input, extra)) return false;
        if (magic != "PhoneRecord-update-v1" || architecture != "arch=x86" || file != "file=PhoneRecord.exe" ||
            versionLine.compare(0, 8, "version=") || sizeLine.compare(0, 5, "size=") || hashLine.compare(0, 7, "sha256=")) return false;
        version = versionLine.substr(8); sha256 = hashLine.substr(7); size = 0;
        std::array<unsigned, 3> parts{};
        if (!updateVersion(version, parts) || sizeLine.size() <= 5 || sizeLine.size() > 13 || sha256.size() != 64) return false;
        for (size_t i = 5; i < sizeLine.size(); ++i) { if (sizeLine[i] < '0' || sizeLine[i] > '9') return false; size = size * 10 + sizeLine[i] - '0'; }
        for (char ch : sha256) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        return size > 0 && size <= 64ULL * 1024 * 1024 && !bytes.empty() && bytes.back() == '\n';
    }
};

/// Read a bounded file without silently accepting truncation or trailing bytes.
static bool updateRead(const wstring& path, string& bytes, size_t limit = 4096) {
    std::ifstream input(path, std::ios::binary | std::ios::ate); if (!input) return false;
    const auto length = input.tellg(); if (length < 0 || static_cast<uint64_t>(length) > limit) return false;
    input.seekg(0); bytes.resize(static_cast<size_t>(length));
    if (!bytes.empty()) input.read(&bytes[0], static_cast<std::streamsize>(bytes.size()));
    return !!input;
}

/// Atomically replace one owned metadata file without touching phone configuration.
static bool updateWrite(const wstring& path, const string& bytes) {
    const wstring pending = path + L".pending";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.flush();
    const bool written = !!output; output.close();
    if (written && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(pending.c_str()); return false;
}

/// Stream SHA-256 while denying writers and deletion; validate exact signed byte count.
static bool updateVerifyFile(const wstring& path, const UpdateManifest& manifest) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; bool valid = false;
    if (GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) == manifest.size &&
        CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        BYTE buffer[65536]; DWORD read = 0; bool good = true;
        while (true) {
            if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) { good = false; break; }
            if (!read) break;
            if (!CryptHashData(hash, buffer, read, 0)) { good = false; break; }
        }
        BYTE digest[32]{}; DWORD length = sizeof(digest); string actual;
        if (good && CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
            for (BYTE byte : digest) { char hex[3]; snprintf(hex, sizeof(hex), "%02x", byte); actual += hex; }
            valid = actual == manifest.sha256;
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file); return valid;
}

/// Persistent UI update preferences; separate from live phone/recording configuration.
struct UpdatePreferences {
    /// Zero disables automatic checks; otherwise one of 1, 6, 12 or 24 hours.
    int hours = 6;
    /// Update balloons are independent of recording/minimize notifications.
    bool balloons = true;
    /// Last automatically notified version; prevents repeated balloons after restart.
    string notified;

    /// Read only validated preference values; corrupted values retain safe defaults.
    void load(const wstring& path) {
        string bytes; if (!updateRead(path, bytes)) return;
        std::istringstream input(bytes); string line;
        while (std::getline(input, line)) {
            if (line == "hours=0") hours = 0;
            else if (line == "hours=1") hours = 1;
            else if (line == "hours=6") hours = 6;
            else if (line == "hours=12") hours = 12;
            else if (line == "hours=24") hours = 24;
            else if (line == "balloons=0") balloons = false;
            else if (line == "balloons=1") balloons = true;
            else if (line.compare(0, 9, "notified=") == 0) { std::array<unsigned, 3> parts{}; if (updateVersion(line.substr(9), parts)) notified = line.substr(9); }
        }
    }

    /// Save update-only preferences atomically; report failures to the operator.
    bool save(const wstring& path) const {
        return updateWrite(path, "hours=" + std::to_string(hours) + "\nballoons=" + (balloons ? "1" : "0") + "\nnotified=" + notified + "\n");
    }
};

/// Monotonic schedule; callers inject time for deterministic sleep/resume tests.
struct UpdateSchedule {
    /// Next due tick; a wake after this tick triggers one check, not a catch-up loop.
    uint64_t next = 0;
    /// Configure startup delay or a newly selected periodic interval.
    void configure(uint64_t now, int hours, bool startup = false) { next = hours > 0 ? now + (startup ? 60000ULL : static_cast<uint64_t>(hours) * 3600000ULL) : 0; }
    /// Claim a single due check and schedule from the current tick.
    bool due(uint64_t now, int hours) { if (hours <= 0 || !next || now < next) return false; configure(now, hours); return true; }
};
