#pragma once

#include "desktop_view.hpp"

#pragma comment(lib, "ole32.lib")

/// Resolve a startup path before changing to the selected configuration's directory.
static wstring absoluteStartupPath(const wstring& path) {
    const DWORD length = GetFullPathNameW(path.c_str(), 0, NULL, NULL);
    if (!length) return path;
    wstring result(length, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), length, &result[0], NULL);
    result.resize(written);
    return result;
}

/// Select per-monitor DPI awareness when available, retaining Windows 7 compatibility.
static void enableDesktopDpi() {
    using Awareness = BOOL(WINAPI*)(HANDLE);
    const auto setAwareness = reinterpret_cast<Awareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setAwareness) setAwareness(reinterpret_cast<HANDLE>(-4)); else SetProcessDPIAware();
}

/// Initialize owned services, run the native desktop or bounded headless diagnostics, then clean up.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments && count > 1 && wcscmp(arguments[1], L"--apply-update") == 0) {
        const int result = updateApply(count, arguments); LocalFree(arguments); return result;
    }
    HANDLE updateAck = NULL;
    wchar_t executable[32768]{}; GetModuleFileNameW(NULL, executable, static_cast<DWORD>(std::size(executable)));
    wstring folder(executable); folder = folder.substr(0, folder.find_last_of(L"\\/"));
    wstring config = folder + L"\\config.ini", exportPath, analysePath, setupReport, setupRestore, setupScript;
    string setupAction, setupPlan;
    bool headless = false, stopExisting = false, startup = false;
    int runFor = 0;
    for (int index = 1; arguments && index < count; ++index) {
        const wstring argument = arguments[index];
        if (argument == L"--config" && index + 1 < count) config = absoluteStartupPath(arguments[++index]);
        else if (argument == L"--offline") g_offline = true;
        else if (argument == L"--headless") headless = true;
        else if (argument == L"--startup") startup = true;
        else if (argument == L"--update-ack" && index + 1 < count) {
            wchar_t* end = NULL; const uint64_t value = _wcstoui64(arguments[++index], &end, 10);
            DWORD flags = 0; HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
            if (value && end && !*end && GetHandleInformation(handle, &flags)) updateAck = handle;
        }
        else if (argument == L"--phone-setup-preview") { setupAction = "preview"; headless = true; }
        else if (argument == L"--phone-setup-apply") { setupAction = "apply"; headless = true; }
        else if (argument == L"--phone-setup-restore" && index + 1 < count) { setupAction = "restore"; setupRestore = absoluteStartupPath(arguments[++index]); headless = true; }
        else if (argument == L"--export-phone-setup-script" && index + 1 < count) { setupAction = "export"; setupScript = absoluteStartupPath(arguments[++index]); headless = true; }
        else if (argument == L"--setup-report" && index + 1 < count) setupReport = absoluteStartupPath(arguments[++index]);
        else if (argument == L"--setup-plan" && index + 1 < count) setupPlan = wideToUtf8(arguments[++index]);
        else if (argument == L"--capture-dir" && index + 1 < count) g_uiCaptureDirectory = absoluteStartupPath(arguments[++index]);
        else if (argument == L"--stop") stopExisting = true;
        else if (argument == L"--run-for" && index + 1 < count) runFor = configurationInteger(wideToUtf8(arguments[++index]));
        else if (argument == L"--export-report" && index + 1 < count) exportPath = absoluteStartupPath(arguments[++index]);
        else if (argument == L"--analyse-wav" && index + 1 < count) analysePath = absoluteStartupPath(arguments[++index]);
        else if (argument == L"--md5" && index + 1 < count) {
            const string digest = Md5::hex(wideToUtf8(arguments[++index]));
            AttachConsole(ATTACH_PARENT_PROCESS); freopen("CONOUT$", "w", stdout); printf("%s\n", digest.c_str()); LocalFree(arguments); return 0;
        }
    }
    if (arguments) LocalFree(arguments);
    if (updateAck) { std::lock_guard<std::mutex> guard(g_updateAdmission); g_updateStartupPending = true; }
    g_configPath = absoluteStartupPath(config);
    folder = g_configPath.substr(0, g_configPath.find_last_of(L"\\/"));
    if (!SetCurrentDirectoryW(folder.c_str())) return 2;
    g_uiLanguage = uiLanguageIndex(loadInterfaceLanguage());
    const string mutexTag = sessionFingerprint(wideToUtf8(g_configPath));
    const wstring mutexName = L"Local\\PhoneRecord2-" + utf8ToWide(mutexTag);
    const wstring stopName = mutexName + L"-stop";
    if (stopExisting) {
        HANDLE target = OpenEventW(EVENT_MODIFY_STATE, FALSE, stopName.c_str());
        if (!target) return 4;
        const bool signaled = SetEvent(target) != FALSE; CloseHandle(target); return signaled ? 0 : 4;
    }
    HANDLE singleton = CreateMutexW(NULL, FALSE, mutexName.c_str());
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!setupAction.empty() && !setupReport.empty()) { PhoneSetupResult result; result.code = "SETUP_CLOSE_APPLICATION"; writePhoneSetupReport(setupReport, result); }
        if (!headless && !startup) MessageBoxW(NULL, uiText("message.instance"), L"PhoneRecord", MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleton); return 3;
    }
    if (!singleton) return 2;
    HANDLE stopSignal = CreateEventW(NULL, TRUE, FALSE, stopName.c_str());
    LOGI("APP_START version=%s pid=%lu offline=%d headless=%d startup=%d", kProductVersion, GetCurrentProcessId(), g_offline, headless, startup);
    TIME_ZONE_INFORMATION zone{}; const DWORD zoneId = GetTimeZoneInformation(&zone);
    LOGI("APP_CLOCK local_timezone_bias_minutes=%ld", zone.Bias + (zoneId == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : zone.StandardBias));
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) { if (stopSignal) CloseHandle(stopSignal); CloseHandle(singleton); return 2; }
    const bool exists = GetFileAttributesW(g_configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    g_cfg.load(wideToUtf8(g_configPath));
    g_healthIntervalSec = g_cfg.healthIntervalSec;
    g_trayNotifications = g_cfg.trayNotifications != 0;
    g_closeAction = g_cfg.closeAction == "exit" ? 2 : g_cfg.closeAction == "minimize" ? 1 : 0;
    if (!setupAction.empty()) {
        int status = 2;
        if (setupAction == "export") status = g_cfg.errors().empty() && writePhoneSetupScript(setupScript) ? 0 : 2;
        else {
            if (setupReport.empty()) setupReport = folder + L"\\phone-setup-report.json";
            status = runPhoneSetupCommand(setupAction, setupPlan, wideToUtf8(setupRestore), setupReport);
        }
        LOGI("PHONE_SETUP_COMMAND_END operation=%s exit=%d", setupAction.c_str(), status);
        WSACleanup(); if (stopSignal) CloseHandle(stopSignal); CloseHandle(singleton); return status;
    }
    initG711();
    g_uiRecordStart = desktopRecordingStarted; g_uiRecordStop = desktopRecordingStopped;
    if (!analysePath.empty()) {
        const bool ok = analyseExistingWav(wideToUtf8(analysePath));
        if (!exportPath.empty()) { std::ofstream output(exportPath, std::ios::binary); output << diagnosticReport(); }
        WSACleanup(); if (stopSignal) CloseHandle(stopSignal); CloseHandle(singleton); return ok ? 0 : 1;
    }
    g_directory.reload();
    const auto invalid = g_cfg.errors();
    std::unique_ptr<HttpServer> events;
    g_commands.start();
    if (exists && invalid.empty()) {
        g_recorder = std::make_unique<Recorder>(); const bool controlReady = g_recorder->start();
        bool eventReady = g_cfg.hookPort == 0;
        if (g_cfg.hookPort > 0) { events = std::make_unique<HttpServer>(g_cfg.hookPort, hookHandler); eventReady = events->start(); }
        if (g_cfg.hookPort == 0) { LOGW("EXTERNAL_TRIGGER_MODE event_listener_disabled=true"); diagnosticUpdate([](DiagnosticSnapshot& state) { if (state.controlListening) state.code = "EXTERNAL_TRIGGER_MODE"; }); }
        if (controlReady && eventReady && !g_offline && g_cfg.hookPort > 0)
            g_commands.enqueue("compatibility", "startup");
        else g_commands.enqueue("local");
        if (g_offline && controlReady && eventReady) diagnosticStage(RecordingStage::Waiting, "OFFLINE_MODE");
    } else {
        diagnosticStage(RecordingStage::Failed, exists ? "CONFIG_INVALID" : "CONFIG_MISSING", invalid.empty() ? "" : invalid.front());
        LOGE("%s", exists ? "CONFIG_INVALID" : "CONFIG_MISSING");
    }
    const ULONGLONG started = GetTickCount64();
    int exitCode = 0;
    if (headless) {
        while (!g_stopping && (!stopSignal || WaitForSingleObject(stopSignal, 100) != WAIT_OBJECT_0) && (runFor <= 0 || GetTickCount64() - started < static_cast<ULONGLONG>(runFor) * 1000)) { if (!stopSignal) Sleep(100); }
        if (!exists || !invalid.empty()) exitCode = 2;
    } else {
        enableDesktopDpi();
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
        HDC screen = GetDC(NULL); const int dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(NULL, screen);
        DesktopView view(dpi, stopSignal);
        WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = DesktopView::procedure; type.hInstance = instance;
        type.hCursor = LoadCursorW(NULL, IDC_ARROW); type.hIcon = applicationIcon(); type.hIconSm = applicationIcon(true); type.lpszClassName = L"PhoneRecord2Window";
        RegisterClassExW(&type);
        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = std::min<int>(MulDiv(1140, dpi, 96), work.right - work.left - 32);
        const int height = std::min<int>(MulDiv(820, dpi, 96), work.bottom - work.top - 32);
        const wstring title = L"PhoneRecord " + utf8ToWide(kProductVersion) + L"  ·  " + uiText("app.subtitle");
        HWND window = CreateWindowExW(0, type.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, NULL, NULL, instance, &view);
        if (window) {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(type.hIcon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(type.hIconSm));
            if (!exists || !invalid.empty()) SendMessageW(window, WM_COMMAND, 103, 0);
            const bool background = startup && exists && invalid.empty() && view.hasTrayIcon();
            ShowWindow(window, background ? SW_HIDE : startup ? SW_SHOWNOACTIVATE : show); UpdateWindow(window);
            if (updateAck) {
                const bool acknowledged = SetEvent(updateAck) != FALSE; CloseHandle(updateAck); updateAck = NULL;
                if (acknowledged) { std::lock_guard<std::mutex> guard(g_updateAdmission); g_updateStartupPending = false; }
            }
            if (startup) LOGI("AUTOSTART_WINDOW hidden=%d tray_available=%d config_valid=%d", background, view.hasTrayIcon(), exists && invalid.empty());
            MSG message{};
            while (GetMessageW(&message, NULL, 0, 0) > 0) {
                if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
                if (runFor > 0 && GetTickCount64() - started >= static_cast<ULONGLONG>(runFor) * 1000) PostMessageW(window, WM_COMMAND, 902, 0);
                if (stopSignal && WaitForSingleObject(stopSignal, 0) == WAIT_OBJECT_0) { ResetEvent(stopSignal); PostMessageW(window, WM_COMMAND, 902, 0); }
            }
        } else { LOGE("WINDOW_CREATE_FAILED win32=%lu", GetLastError()); exitCode = 2; }
        CoUninitialize();
    }
    if (!exportPath.empty()) {
        std::ofstream output(exportPath, std::ios::binary | std::ios::trunc); output << diagnosticReport(); output.flush();
        if (!output) { LOGE("REPORT_EXPORT_FAILED"); exitCode = 2; }
    }
    g_callEnded = true;
    if (events) events->shutdown();
    g_commands.shutdown(true);
    g_stopping = true;
    if (g_recorder) { g_recorder->shutdown(); g_recorder.reset(); }
    LOGI("APP_STOP workers_joined=true");
    WSACleanup();
    if (stopSignal) CloseHandle(stopSignal);
    CloseHandle(singleton);
    return exitCode;
}
