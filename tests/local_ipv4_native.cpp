#define PHONERECORD_NETWORK_TESTING
#include "../cpp/phonerecord.cpp"
#include <iostream>
#include <stdexcept>

/// Fail with synthetic case labels, never with real adapter or user information.
static void requireLocalIp(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}

/// Exercise the actual draft methods and native edit notifications without creating a product session.
struct LocalIpv4Fixture {
    /// Forward only the two settings notifications and the lookup-completion message.
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* view = reinterpret_cast<DesktopView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            view = static_cast<DesktopView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        }
        if (view && (message == WM_COMMAND || message == WM_APP + 25)) return view->handle(message, wparam, lparam);
        return DefWindowProcW(window, message, wparam, lparam);
    }

    /// Drain only this thread's owned windows until all suggestion work completes.
    static void settle(DesktopView& view) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        do {
            MSG message{};
            while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (!view.localIpWorker_.joinable()) return;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("local suggestion completion timed out");
    }

    /// Cover saved/manual ownership, stale completion, routing refresh and offline isolation.
    static void run() {
        DesktopView view(96);
        WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(NULL); type.lpszClassName = L"PhoneRecordLocalIpTest";
        RegisterClassW(&type);
        view.window_ = CreateWindowW(type.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, type.hInstance, &view);
        requireLocalIp(view.window_ != NULL, "owned parent");
        view.fields_[0] = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 50, 20, view.window_, reinterpret_cast<HMENU>(400), type.hInstance, NULL);
        view.fields_[4] = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 20, 50, 20, view.window_, reinterpret_cast<HMENU>(404), type.hInstance, NULL);
        int calls = 0;
        g_localIpv4TestResolver = [&](const string& phone) {
            ++calls;
            return phone == "198.51.100.9" ? string() : phone.empty() ? string("192.0.2.20") : phone == "198.51.100.1" ? string("192.0.2.21") : string("192.0.2.22");
        };
        const string saved = g_cfg.advertiseIp;
        view.requestLocalIpSuggestion(); settle(view);
        requireLocalIp(view.text(view.fields_[4]) == "192.0.2.20", "blank draft default");
        requireLocalIp(g_cfg.advertiseIp == saved, "suggestion never mutates live configuration");

        SetWindowTextW(view.fields_[0], L"198.51.100.1");
        SendMessageW(view.window_, WM_COMMAND, MAKEWPARAM(400, EN_KILLFOCUS), 0); settle(view);
        requireLocalIp(view.text(view.fields_[4]) == "192.0.2.21", "phone route replaces untouched default");

        SetWindowTextW(view.fields_[4], L"192.0.2.80");
        SetWindowTextW(view.fields_[0], L"198.51.100.2");
        const int beforeManual = calls;
        view.requestLocalIpSuggestion(); settle(view);
        requireLocalIp(view.text(view.fields_[4]) == "192.0.2.80" && calls == beforeManual, "manual or saved value preserved");

        SetWindowTextW(view.fields_[4], L""); view.requestLocalIpSuggestion();
        SetWindowTextW(view.fields_[4], L"192.0.2.81"); settle(view);
        requireLocalIp(view.text(view.fields_[4]) == "192.0.2.81", "manual edit wins in-flight lookup");

        SetWindowTextW(view.fields_[4], L""); SetWindowTextW(view.fields_[0], L"198.51.100.1");
        view.requestLocalIpSuggestion(); SetWindowTextW(view.fields_[0], L"198.51.100.2"); settle(view);
        requireLocalIp(view.text(view.fields_[4]) == "192.0.2.22", "stale phone result discarded and refreshed");

        SetWindowTextW(view.fields_[0], L"198.51.100.9"); view.requestLocalIpSuggestion(); settle(view);
        requireLocalIp(view.text(view.fields_[4]).empty(), "unknown route clears only previous suggestion");

        g_offline = true; const int beforeOffline = calls;
        view.requestLocalIpSuggestion(); settle(view);
        requireLocalIp(calls == beforeOffline, "offline mode does not enumerate real adapters");
        g_offline = false;
        SetWindowTextW(view.fields_[0], L"not-an-ip"); view.requestLocalIpSuggestion(); settle(view);
        requireLocalIp(calls == beforeOffline, "incomplete phone input never becomes a route query");
        SetWindowTextW(view.fields_[0], L""); view.requestLocalIpSuggestion();
        if (view.localIpWorker_.joinable()) view.localIpWorker_.join();
        DestroyWindow(view.window_);
        g_localIpv4TestResolver = {};
    }
};

/// Validate deterministic address choice and the real UI ownership/completion path.
int main() {
    try {
        WSADATA data{}; requireLocalIp(WSAStartup(MAKEWORD(2, 2), &data) == 0, "winsock startup");
        for (const char* value : {"", "0.0.0.0", "127.0.0.1", "169.254.1.2", "224.0.0.1", "255.255.255.255", "bad"})
            requireLocalIp(!usableLocalIpv4(value), "unusable address rejected");
        const std::vector<LocalIpv4Candidate> adapters = {{"192.0.2.20", 20, true}, {"198.51.100.20", 10, true}, {"203.0.113.20", 1, false}};
        requireLocalIp(selectLocalIpv4(adapters) == "198.51.100.20", "unique lowest metric default");
        requireLocalIp(selectLocalIpv4(adapters, "203.0.113.20", true) == "203.0.113.20", "phone route preferred over default interface");
        requireLocalIp(selectLocalIpv4(adapters, "192.0.2.99", true).empty(), "route source must be assigned");
        requireLocalIp(selectLocalIpv4(adapters, "", true).empty(), "route failure does not guess default");
        requireLocalIp(selectLocalIpv4({{"192.0.2.20", 10, true}, {"198.51.100.20", 10, true}}).empty(), "ambiguous default stays blank");
        requireLocalIp(selectLocalIpv4({{"192.0.2.20", 1, false}}) == "192.0.2.20", "single isolated interface");
        requireLocalIp(selectLocalIpv4({{"192.0.2.20", 1, false}, {"198.51.100.20", 1, false}}).empty(), "multiple isolated interfaces stay blank");
        requireLocalIp(selectLocalIpv4({{"127.0.0.1", 0, true}, {"169.254.2.3", 0, true}}).empty(), "unusable candidates filtered");
        LocalIpv4Fixture::run();
        WSACleanup();
        std::cout << "LOCAL_IPV4_NATIVE_OK selection=8 draft_and_lifecycle=9 synthetic_only=true\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
