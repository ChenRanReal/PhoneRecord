#pragma once

#include <windows.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

/// Observed current-user Windows registration, independent of a copied config.ini.
struct UserStartupState {
    bool enabled = false;
    bool thisCopy = false;
    LONG error = ERROR_SUCCESS;
};

/// Own only the PhoneRecord value in the current user's native Windows Run key.
class UserStartup {
public:
    /// Bind startup to absolute executable/configuration paths without a shell or credentials.
    UserStartup(const std::wstring& executable, const std::wstring& configuration) {
        if (!executable.empty() && !configuration.empty() &&
            executable.find_first_of(L"\"\r\n") == std::wstring::npos &&
            configuration.find_first_of(L"\"\r\n") == std::wstring::npos &&
            executable.back() != L'\\' && configuration.back() != L'\\') {
            command_ = L"\"" + executable + L"\" --config \"" + configuration + L"\" --startup";
            // Windows documents a maximum of 260 characters for a Run command.
            if (command_.size() > 260) command_.clear();
        }
    }

    /// Read the operating system rather than trusting a portable settings-file flag.
    UserStartupState read() const {
        UserStartupState state;
        HKEY key = NULL;
        LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (result == ERROR_FILE_NOT_FOUND) return state;
        if (result != ERROR_SUCCESS) { state.error = result; return state; }
        DWORD type = 0, bytes = 0;
        result = RegQueryValueExW(key, valueName(), NULL, &type, NULL, &bytes);
        if (result == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return state; }
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536 || bytes % sizeof(wchar_t)) {
            RegCloseKey(key); state.error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result; return state;
        }
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        result = RegQueryValueExW(key, valueName(), NULL, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) { state.error = result; return state; }
        const std::wstring actual(buffer.data());
        state.enabled = !actual.empty();
        state.thisCopy = state.enabled && !command_.empty() && _wcsicmp(actual.c_str(), command_.c_str()) == 0;
        return state;
    }

    /// Register or remove this product only, then verify the resulting Windows value.
    LONG setEnabled(bool enabled) const {
        if (enabled && command_.empty()) return ERROR_FILENAME_EXCED_RANGE;
        HKEY key = NULL;
        LONG result = enabled
            ? RegCreateKeyExW(HKEY_CURRENT_USER, keyPath(), 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL)
            : RegOpenKeyExW(HKEY_CURRENT_USER, keyPath(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (!enabled && result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS) return result;
        if (enabled) result = RegSetValueExW(key, valueName(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command_.c_str()),
            static_cast<DWORD>((command_.size() + 1) * sizeof(wchar_t)));
        else { result = RegDeleteValueW(key, valueName()); if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS; }
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) return result;
        const UserStartupState actual = read();
        if (actual.error != ERROR_SUCCESS) return actual.error;
        return actual.enabled == enabled && (!enabled || actual.thisCopy) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    }

    /// Return the one product-owned value name for tests and operational documentation.
    static const wchar_t* valueName() { return L"PhoneRecord"; }

    /// Return the documented per-user login location, using one view on x86/x64 Windows.
    static const wchar_t* keyPath() { return L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; }

private:
    /// Fully quoted direct executable invocation, or empty when it cannot be registered safely.
    std::wstring command_;
};
