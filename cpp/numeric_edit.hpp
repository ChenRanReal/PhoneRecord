#pragma once

/// Keep native numeric edits strict across keyboard input, paste and replacement operations.
class NumericEdit {
public:
    /// Attach a bounded positive-integer policy without changing programmatic config loading.
    static void attach(HWND window, unsigned maximum) {
        SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | ES_NUMBER);
        SendMessageW(window, EM_SETLIMITTEXT, 5, 0);
        SetWindowSubclass(window, procedure, 1, maximum);
    }

private:
    /// Validate the complete value after replacing the current selection; empty is a typing draft.
    static bool accepts(HWND window, const wstring& insert, unsigned maximum) {
        const int length = GetWindowTextLengthW(window);
        wstring value(static_cast<size_t>(length) + 1, L'\0'); GetWindowTextW(window, &value[0], length + 1); value.resize(length);
        DWORD start = 0, end = 0; SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        if (start > value.size() || end > value.size() || end < start) return false;
        value.replace(start, end - start, insert);
        if (value.empty()) return true;
        if (value.size() > 5) return false;
        unsigned number = 0;
        for (wchar_t byte : value) { if (byte < L'0' || byte > L'9') return false; number = number * 10 + byte - L'0'; }
        return number > 0 && number <= maximum;
    }

    /// Reject a whole invalid paste rather than silently changing its numeric meaning.
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR policy) {
        const unsigned maximum = static_cast<unsigned>(policy);
        if (message == WM_CHAR && wparam >= 0x20 && !accepts(window, wstring(1, static_cast<wchar_t>(wparam)), maximum)) { MessageBeep(MB_ICONWARNING); return 0; }
        if (message == EM_REPLACESEL) {
            const wchar_t* replacement = reinterpret_cast<const wchar_t*>(lparam);
            if (replacement && !accepts(window, replacement, maximum)) return 0;
        }
        if (message == WM_PASTE) {
            if (!OpenClipboard(window)) return 0;
            HANDLE memory = GetClipboardData(CF_UNICODETEXT);
            wstring value;
            if (memory) {
                const SIZE_T bytes = GlobalSize(memory);
                const wchar_t* content = static_cast<const wchar_t*>(GlobalLock(memory));
                if (content && bytes >= sizeof(wchar_t) && bytes <= 65536) {
                    const size_t count = wcsnlen_s(content, bytes / sizeof(wchar_t));
                    if (count < bytes / sizeof(wchar_t)) value.assign(content, count);
                }
                if (content) GlobalUnlock(memory);
            }
            CloseClipboard();
            if (!value.empty() && accepts(window, value, maximum)) return DefSubclassProc(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(value.c_str()));
            MessageBeep(MB_ICONWARNING); return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, procedure, id);
        return DefSubclassProc(window, message, wparam, lparam);
    }
};
