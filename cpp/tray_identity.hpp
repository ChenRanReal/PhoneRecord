#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <cstring>

/// Assign a stable path-scoped GUID, returning false if Windows cannot derive it.
/// In-place updates retain this identity; portable copies must not claim another path's GUID.
/// The hash excludes executable contents, version, language and window handles.
static bool initializeTrayIdentity(NOTIFYICONDATAW& icon) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(NULL, path, 32768);
    if (!length || length >= 32768) return false;
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path,
        static_cast<int>(length), path, static_cast<int>(length), NULL, NULL, 0)) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    BYTE digest[32]{};
    DWORD size = sizeof(digest);
    const wchar_t domain[] = L"PhoneRecord.notification-area.v1";
    const bool success = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(domain), sizeof(domain), 0) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(path), length * sizeof(wchar_t), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!success) return false;
    memcpy(&icon.guidItem, digest, sizeof(icon.guidItem));
    // RFC 9562 version 8 UUID: application-defined deterministic identity.
    icon.guidItem.Data3 = (icon.guidItem.Data3 & 0x0fff) | 0x8000;
    icon.guidItem.Data4[0] = (icon.guidItem.Data4[0] & 0x3f) | 0x80;
    icon.uFlags |= NIF_GUID;
    return true;
}
