#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../cpp/user_startup.hpp"

/// Fail with a stable synthetic-test label, never with registry contents or personal paths.
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/// Redirect this process's HKCU into a uniquely owned disposable test namespace.
class IsolatedUserHive {
public:
    /// Create a fresh namespace without ever redirecting another process's registry view.
    IsolatedUserHive() {
        path_ = L"Software\\PhoneRecord.Tests\\Startup-" + std::to_wstring(GetCurrentProcessId());
        DWORD disposition = 0;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
            NULL, &key_, &disposition) == ERROR_SUCCESS, "create isolated hive");
        require(disposition == REG_CREATED_NEW_KEY, "test namespace already exists");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, key_) == ERROR_SUCCESS, "redirect test hive");
    }

    /// Restore the real root before deleting only the test's own uniquely named subkey.
    ~IsolatedUserHive() {
        RegOverridePredefKey(HKEY_CURRENT_USER, NULL);
        if (key_) RegCloseKey(key_);
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }
private:
    HKEY key_ = NULL;
    std::wstring path_;
};

/// Read the actual native registry value used by an isolated test, outside the product helper.
static std::wstring readValue(HKEY key, const wchar_t* name) {
    wchar_t value[1024]{}; DWORD bytes = sizeof(value), type = 0;
    require(RegQueryValueExW(key, name, NULL, &type, reinterpret_cast<BYTE*>(value), &bytes) == ERROR_SUCCESS,
        "read registry value");
    require(type == REG_SZ, "registry string type");
    return value;
}

/// Exercise real Windows registry success, preservation, limits and access-denied paths in isolation.
int main() {
    try {
        IsolatedUserHive isolated;
        UserStartup current(L"C:\\電話 測試 & %!\\PhoneRecord.exe", L"C:\\設定 測試\\config.ini");
        require(!current.read().enabled && !current.read().error, "missing startup is off");
        require(current.setEnabled(false) == ERROR_SUCCESS, "idempotent off with missing key");
        require(current.setEnabled(true) == ERROR_SUCCESS && current.read().thisCopy, "enable with readback");
        HKEY run = NULL;
        require(RegOpenKeyExW(HKEY_CURRENT_USER, UserStartup::keyPath(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
            &run) == ERROR_SUCCESS, "open isolated Run key");
        const auto expected = L"\"C:\\電話 測試 & %!\\PhoneRecord.exe\" --config \"C:\\設定 測試\\config.ini\" --startup";
        require(readValue(run, UserStartup::valueName()) == expected, "quoted Unicode direct invocation");
        const wchar_t neighbor[] = L"synthetic unrelated value";
        require(RegSetValueExW(run, L"UnrelatedApplication", 0, REG_SZ, reinterpret_cast<const BYTE*>(neighbor),
            sizeof(neighbor)) == ERROR_SUCCESS, "prepare unrelated value");
        require(current.setEnabled(true) == ERROR_SUCCESS && readValue(run, L"UnrelatedApplication") == neighbor,
            "repeated enable preserves unrelated values");
        UserStartup moved(L"C:\\another location\\PhoneRecord.exe", L"C:\\another config\\config.ini");
        require(moved.read().enabled && !moved.read().thisCopy, "identify a different registered copy");
        require(moved.setEnabled(true) == ERROR_SUCCESS && moved.read().thisCopy && !current.read().thisCopy,
            "explicit transfer to a moved copy");
        require(current.setEnabled(false) == ERROR_SUCCESS && !current.read().enabled &&
            readValue(run, L"UnrelatedApplication") == neighbor, "off removes this product only");
        UserStartup longPath(std::wstring(280, L'x'), L"C:\\config.ini");
        require(longPath.setEnabled(true) == ERROR_FILENAME_EXCED_RANGE && !current.read().enabled,
            "reject oversized Run command without writing");
        const DWORD unsupported = 7;
        RegSetValueExW(run, UserStartup::valueName(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&unsupported), sizeof(unsupported));
        require(current.read().error == ERROR_INVALID_DATA, "malformed registration is not reported as off");
        RegDeleteValueW(run, UserStartup::valueName());
        require(current.setEnabled(true) == ERROR_SUCCESS, "prepare permission regression");
        DWORD securityBytes = 0;
        RegGetKeySecurity(run, DACL_SECURITY_INFORMATION, NULL, &securityBytes);
        std::vector<BYTE> originalSecurity(securityBytes);
        require(RegGetKeySecurity(run, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(originalSecurity.data()), &securityBytes) == ERROR_SUCCESS,
            "backup isolated key ACL");
        ACL deny{}; SECURITY_DESCRIPTOR descriptor{};
        InitializeAcl(&deny, sizeof(deny), ACL_REVISION);
        InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor, TRUE, &deny, FALSE);
        require(RegSetKeySecurity(run, DACL_SECURITY_INFORMATION, &descriptor) == ERROR_SUCCESS, "deny isolated key");
        const LONG readError = current.read().error, writeError = current.setEnabled(true), deleteError = current.setEnabled(false);
        const LONG restored = RegSetKeySecurity(run, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(originalSecurity.data()));
        require(restored == ERROR_SUCCESS, "restore isolated ACL");
        require(readError == ERROR_ACCESS_DENIED && writeError == ERROR_ACCESS_DENIED && deleteError == ERROR_ACCESS_DENIED,
            "permission errors are explicit");
        require(current.read().thisCopy && readValue(run, L"UnrelatedApplication") == neighbor,
            "denied changes preserve all prior values");
        RegCloseKey(run);
        std::cout << "{\"passed\":true,\"isolated_registry\":true,\"checks\":8}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
