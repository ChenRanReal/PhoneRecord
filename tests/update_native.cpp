/// Native update regressions use ephemeral trust and in-memory HTTP fixtures, never a real phone or release.
#define PHONERECORD_UPDATE_TESTING
#define WinMain PhoneRecordProductMain
#include "../cpp/phonerecord.cpp"
#undef WinMain
#include <stdexcept>

/// Fail with a stable technical label, without logging test key material.
static void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }

/// Ephemeral RSA signing fixture, destroyed with its provider and never exported privately.
class SigningFixture {
public:
    /// Generate the fixture trust root and export only its public CryptoAPI blob.
    SigningFixture() {
        require(CryptAcquireContextW(&provider_, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE, "fixture provider");
        require(CryptGenKey(provider_, AT_SIGNATURE, (2048 << 16), &key_) != FALSE, "fixture key");
        DWORD size = 0; require(CryptExportKey(key_, 0, PUBLICKEYBLOB, 0, NULL, &size) != FALSE, "public size");
        std::vector<BYTE> bytes(size); require(CryptExportKey(key_, 0, PUBLICKEYBLOB, 0, bytes.data(), &size) != FALSE, "public export");
        g_updateTestKey = base64(bytes);
    }
    /// Release both key and provider, including failure paths after test assertions.
    ~SigningFixture() { if (key_) CryptDestroyKey(key_); if (provider_) CryptReleaseContext(provider_, 0); }
    /// Encode public signatures in the same big-endian format as the Actions signer.
    string sign(const string& data) {
        HCRYPTHASH hash = 0; require(CryptCreateHash(provider_, CALG_SHA_256, 0, 0, &hash) != FALSE, "fixture hash");
        require(CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size()), 0) != FALSE, "fixture data");
        DWORD length = 0; require(CryptSignHashW(hash, AT_SIGNATURE, NULL, 0, NULL, &length) != FALSE, "signature size");
        std::vector<BYTE> bytes(length); require(CryptSignHashW(hash, AT_SIGNATURE, NULL, 0, bytes.data(), &length) != FALSE, "signature");
        CryptDestroyHash(hash); std::reverse(bytes.begin(), bytes.end()); return base64(bytes);
    }
    /// Produce standard base64 without CRLF or embedded terminal data.
    static string base64(const std::vector<BYTE>& data) {
        DWORD length = 0; CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &length);
        string result(length, '\0'); require(CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &length) != FALSE, "base64");
        result.resize(length); while (!result.empty() && result.back() == '\0') result.pop_back(); return result;
    }
    /// Hash a bounded fixture executable using the same Windows provider as the product.
    static string digest(const string& data) {
        HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
        require(CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash), "fixture digest provider");
        require(CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size()), 0) != FALSE, "fixture digest bytes");
        BYTE bytes[32]{}; DWORD length = 32; require(CryptGetHashParam(hash, HP_HASHVAL, bytes, &length, 0) != FALSE, "fixture digest");
        string result; for (BYTE byte : bytes) { char hex[3]; snprintf(hex, sizeof(hex), "%02x", byte); result += hex; }
        CryptDestroyHash(hash); CryptReleaseContext(provider, 0); return result;
    }
private:
    /// Owned fixture provider and non-exported private signing key.
    HCRYPTPROV provider_ = 0;
    HCRYPTKEY key_ = 0;
};

/// Wait for a concrete asynchronous state transition with a short deterministic deadline.
static void awaitState(UpdateService& service, const char* expected) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) { const auto state = service.state(); if (!state.busy && state.status == expected) return; Sleep(10); }
    throw std::runtime_error(string("state transition: ") + expected + " actual=" + service.state().status);
}

/// Inspect the real UI/tray state without sending a system balloon or touching another application's icon.
struct UpdateTrayFixture {
    /// Validate the persistent arrow, tooltip, shell recreation and update-balloon click routing.
    static void run(const wstring& folder, const string& manifest, const string& signature) {
        g_configPath = folder + L"\\tray.ini"; g_offline = true; g_uiLanguage = 2;
        UpdatePreferences preferences; preferences.hours = 0; preferences.balloons = false;
        require(preferences.save(g_configPath + L".updates") && updateWrite(g_configPath + L".update-cache", manifest + "\n" + signature), "tray cache fixture");
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
        {
            DesktopView view(96); WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = DesktopView::procedure; type.hInstance = GetModuleHandleW(NULL); type.lpszClassName = L"PhoneRecordUpdateNativeTrayFixture";
            require(RegisterClassExW(&type) != 0, "fixture UI class");
            HWND window = CreateWindowExW(0, type.lpszClassName, L"PhoneRecord update fixture", WS_OVERLAPPEDWINDOW, 0, 0, 980, 760, NULL, NULL, type.hInstance, &view);
            require(window != NULL, "fixture native window");
            require(view.updateIcon_ != NULL && view.tray_.hIcon == view.updateIcon_ && wstring(view.tray_.szTip).find(L"9.0.0") != wstring::npos, "cached update selects arrow icon and version tooltip while hidden");
            ICONINFO icon{}; require(GetIconInfo(view.updateIcon_, &icon) != FALSE, "owned update icon");
            BITMAP bitmap{}; GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap);
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = bitmap.bmWidth; info.bmiHeader.biHeight = -bitmap.bmHeight; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            std::vector<DWORD> pixels(bitmap.bmWidth * bitmap.bmHeight); HDC dc = GetDC(window);
            require(GetDIBits(dc, icon.hbmColor, 0, bitmap.bmHeight, pixels.data(), &info, DIB_RGB_COLORS) != 0, "read native arrow icon pixels");
            ReleaseDC(window, dc); DeleteObject(icon.hbmColor); DeleteObject(icon.hbmMask);
            require(std::count(pixels.begin(), pixels.end(), 0xff087fbd) > 0 && std::count(pixels.begin(), pixels.end(), 0xffffffff) > 0, "opaque blue badge and white arrow pixels");
            require(view.trayAdded_, "fixture icon registered");
            Shell_NotifyIconW(NIM_DELETE, &view.tray_); view.trayAdded_ = false;
            SendMessageW(window, view.taskbarCreated_, 0, 0);
            require(view.trayAdded_ && view.tray_.hIcon == view.updateIcon_, "Explorer recreation restores the same update icon");
            view.updateBalloonActive_ = true; view.trayBalloonActive_ = true;
            SendMessageW(window, WM_APP + 31, 0, NIN_BALLOONUSERCLICK);
            require(view.page_ == 5 && !view.updateBalloonActive_, "update balloon click routes to updates rather than recording page");
            ShowWindow(window, SW_HIDE); DestroyWindow(window); UnregisterClassW(type.lpszClassName, type.hInstance);
        }
        CoUninitialize();
        DeleteFileW((g_configPath + L".updates").c_str()); DeleteFileW((g_configPath + L".update-cache").c_str());
    }
};

/// Count one exact badge color in an owned icon. The shared product icon is not modified.
static int iconPixels(HWND window, HICON icon, DWORD color) {
    ICONINFO info{}; require(GetIconInfo(icon, &info) != FALSE, "owned alert icon");
    BITMAP bitmap{}; GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
    BITMAPINFO header{}; header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); header.bmiHeader.biWidth = bitmap.bmWidth; header.bmiHeader.biHeight = -bitmap.bmHeight; header.bmiHeader.biPlanes = 1; header.bmiHeader.biBitCount = 32;
    std::vector<DWORD> pixels(bitmap.bmWidth * bitmap.bmHeight); HDC dc = GetDC(window);
    require(GetDIBits(dc, info.hbmColor, 0, bitmap.bmHeight, pixels.data(), &header, DIB_RGB_COLORS) != 0, "read alert icon pixels");
    ReleaseDC(window, dc); DeleteObject(info.hbmColor); DeleteObject(info.hbmMask);
    return static_cast<int>(std::count(pixels.begin(), pixels.end(), color));
}

/// DHCP address loss overrides the update arrow and clears again when the phone is usable.
struct AddressAlertFixture {
    /// Drive the real tray and window icons through health changes without contacting a phone.
    static void run(const wstring& folder) {
        g_configPath = folder + L"\\alert.ini"; g_offline = false; g_uiLanguage = 2; g_cfg.phoneAddressMode = "dhcp";
        IN_ADDR address{}; require(InetPtonA(AF_INET, "192.0.2.20", &address) == 1, "synthetic phone address");
        g_phoneResolved = address.S_un.S_addr;
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.phoneHealth.status = 0; state.phoneHealth.code.clear(); });
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
        DesktopView view(96); WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = DesktopView::procedure; type.hInstance = GetModuleHandleW(NULL); type.lpszClassName = L"PhoneRecordAddressAlertFixture";
        require(RegisterClassExW(&type) != 0, "alert UI class");
        HWND window = CreateWindowExW(0, type.lpszClassName, L"PhoneRecord alert fixture", WS_OVERLAPPEDWINDOW, 0, 0, 980, 760, NULL, NULL, type.hInstance, &view);
        require(window != NULL, "alert native window");
        require(view.alertIcon_ == NULL && view.tray_.hIcon != NULL, "known DHCP address stays unmarked before the first health result");
        diagnosticHealth(true, false, "PHONE_UNREACHABLE");
        SendMessageW(window, WM_TIMER, 1, 0);
        require(view.alertIcon_ != NULL && view.tray_.hIcon == view.alertIcon_, "unreachable DHCP target selects the red exclamation");
        require(wstring(view.tray_.szTip).find(L"recording is unavailable") != wstring::npos, "tray tip explains that recording is unavailable");
        require(iconPixels(window, view.alertIcon_, 0xffd1242f) > 8 && iconPixels(window, view.alertIcon_, 0xffffffff) > 0, "opaque red badge and white exclamation");
        require(reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0)) == view.alertIconLarge_, "window icon uses the same warning");
        diagnosticHealth(true, true, "PHONE_OK");
        SendMessageW(window, WM_TIMER, 1, 0);
        require(view.tray_.hIcon != view.alertIcon_ && !view.alertShown_, "reachable phone restores the normal icon");
        g_phoneResolved = 0;
        SendMessageW(window, WM_TIMER, 1, 0);
        require(view.tray_.hIcon == view.alertIcon_, "missing DHCP address is unusable");
        g_phoneResolved = address.S_un.S_addr;
        diagnosticHealth(true, false, "AUTH_REJECTED");
        SendMessageW(window, WM_TIMER, 1, 0);
        require(view.tray_.hIcon != view.alertIcon_, "credential failure is not an address-change warning");
        g_cfg.phoneAddressMode = "fixed"; g_phoneResolved = 0; diagnosticHealth(true, false, "PHONE_UNREACHABLE");
        SendMessageW(window, WM_TIMER, 1, 0);
        require(view.tray_.hIcon != view.alertIcon_, "fixed mode keeps its own address failure path");
        ShowWindow(window, SW_HIDE); DestroyWindow(window); UnregisterClassW(type.lpszClassName, type.hInstance);
        CoUninitialize();
        g_cfg.phoneAddressMode = "fixed"; g_offline = true;
    }
};

/// Exercise semantic versions, signed manifests, bounded I/O, live scheduling, caching and download integrity.
int wmain(int count, wchar_t** args) {
    try {
        if (count > 1 && wcscmp(args[1], L"--apply-update") == 0) {
            wchar_t publicKey[8192]{}; GetEnvironmentVariableW(L"PHONERECORD_UPDATE_TEST_PUBLIC_KEY", publicKey, 8192); g_updateTestKey = wideToUtf8(publicKey);
            return updateApply(count, args);
        }
        if (count > 2 && wcscmp(args[1], L"--test-install-parent") == 0) {
            wchar_t publicKey[8192]{}; GetEnvironmentVariableW(L"PHONERECORD_UPDATE_TEST_PUBLIC_KEY", publicKey, 8192); g_updateTestKey = wideToUtf8(publicKey);
            const wstring directory = args[2]; UpdateState state;
            require(updateRead(directory + L"\\stage\\update-manifest.txt", state.signedManifest) && updateRead(directory + L"\\stage\\update-manifest.sig", state.signature, 8192) && state.manifest.parse(state.signedManifest), "install parent fixture");
            state.staged = directory + L"\\stage\\PhoneRecord.exe";
            require(updateBeginInstall(state, directory + L"\\config.ini"), "installer parent/helper handshake"); return 0;
        }
        if (count > 2 && wcscmp(args[1], L"--config") == 0) {
            const wstring config = args[2];
            for (int i = 3; i + 1 < count; ++i) if (wcscmp(args[i], L"--update-ack") == 0) {
                if (GetFileAttributesW((config + L".fail-startup").c_str()) != INVALID_FILE_ATTRIBUTES) return 77;
                HANDLE ack = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(args[++i], NULL, 10)));
                require(SetEvent(ack) != FALSE, "new child startup acknowledgment"); CloseHandle(ack);
            }
            require(updateWrite(config + L".child-started", "started\n"), "fixture child started"); return 0;
        }
        require(count == 2 || (count == 3 && wcscmp(args[2], L"--skip-tray") == 0), "test workspace argument");
        const wstring folder = args[1];
        SigningFixture key;
        string interop;
        require(updateRead(folder + L"\\interop.json", interop, 16384) && updateSignature(jsonField(interop, "manifest"), jsonField(interop, "signature"), jsonField(interop, "publicKey").c_str()), ".NET Actions signer/CryptoAPI interoperability");
        std::array<unsigned, 3> parts{};
        require(updateNewer("2.10.0", "2.9.9") && !updateNewer("2.6.0", "2.6.0"), "numeric version comparison");
        for (const string& bad : {"2.6", "2.6.0.0", "02.6.0", "2.6.0-beta", "2.65536.0", "../2.6.0", "2..0"}) require(!updateVersion(bad, parts), "invalid version rejection");
        UpdateSchedule schedule; schedule.configure(100, 6, true);
        require(!schedule.due(60099, 6) && schedule.due(60100, 6), "startup delay");
        require(schedule.due(999999999, 6) && !schedule.due(999999999, 6), "resume coalesces missed intervals");
        schedule.configure(100, 0); require(!schedule.due(999999999, 0), "automatic checks disabled");

        const string payload = "abc";
        const string manifest = "PhoneRecord-update-v1\nversion=9.0.0\narch=x86\nfile=PhoneRecord.exe\nsize=3\nsha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
        const string signature = key.sign(manifest); UpdateManifest parsed;
        require(updateSignature(manifest, signature) && parsed.parse(manifest), "publisher signature interoperability");
        require(!updateSignature(manifest + "x", signature) && !updateSignature(manifest, signature.substr(1)), "tampered and malformed signatures rejected");
        require(!parsed.parse(manifest + "size=3\n") && !parsed.parse(string("\xef\xbb\xbf") + manifest), "duplicate fields and BOM rejected");
        require(parsed.parse(manifest), "canonical manifest restores");
        const wstring binary = folder + L"\\payload.exe";
        require(updateWrite(binary, payload) && updateVerifyFile(binary, parsed), "streamed signed digest");
        require(updateWrite(binary, "abd") && !updateVerifyFile(binary, parsed), "same-size digest mismatch");
        require(updateWrite(binary, "ab") && !updateVerifyFile(binary, parsed), "truncated executable rejected");
        string metadataBytes; require(!updateRead(binary, metadataBytes, 1), "bounded metadata read"); DeleteFileW(binary.c_str());
        const wstring quoted = LR"(C:\測試 \)"; int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW((L"test.exe " + updateArgument(quoted)).c_str(), &argumentCount);
        require(arguments && argumentCount == 2 && quoted == arguments[1], "Windows Unicode/trailing slash quoting"); LocalFree(arguments);

        const wstring config = folder + L"\\config.ini";
        std::atomic<int> calls{0}, response{200}; std::atomic<bool> corrupt{false};
        g_updateTestHttp = [&](const wstring& url, string& body, size_t limit, const wstring& destination, const std::function<void(size_t)>& progress) -> DWORD {
            ++calls;
            if (url.find(L"api.github.com") != wstring::npos) {
                body = "{\"tag_name\":\"v9.0.0\",\"body\":\"Synthetic release \\u66f4\\u65b0\",\"assets\":[";
                bool first = true;
                for (const wchar_t* name : {L"PhoneRecord.exe", L"PhoneRecord.exe.sha256", L"update-manifest.txt", L"update-manifest.sig"}) {
                    if (!first) body += ','; first = false;
                    body += "{\"browser_download_url\":" + jsonQuoted(wideToUtf8(UpdateService::releaseUrl("9.0.0", name))) + "}";
                }
                body += "]}"; return response.load();
            }
            if (url.find(L"update-manifest.txt") != wstring::npos) { body = manifest; return 200; }
            if (url.find(L"update-manifest.sig") != wstring::npos) { body = signature; return 200; }
            require(limit == 3 && !destination.empty(), "bounded executable staging");
            require(updateWrite(destination, corrupt ? "abd" : payload), "fixture download write"); if (progress) progress(3); return 200;
        };
        {
            UpdateService service; service.start(NULL, config, false); service.testDueSoon(); awaitState(service, "update.available");
            require(calls == 3 && service.state().manifest.version == "9.0.0", "live periodic check without restart");
            require(service.markNotified("9.0.0") && service.configure(0, false), "independent preferences persisted");
            service.request(true); awaitState(service, "update.ready");
            const auto state = service.state(); require(updateVerifyFile(state.staged, state.manifest), "downloaded executable verified before ready");
            const wstring directory = state.staged.substr(0, state.staged.find_last_of(L"\\"));
            DeleteFileW(state.staged.c_str()); DeleteFileW((directory + L"\\update-manifest.txt").c_str()); DeleteFileW((directory + L"\\update-manifest.sig").c_str()); RemoveDirectoryW(directory.c_str());
        }
        {
            const int before = calls; UpdateService service; service.start(NULL, config, false);
            require(service.state().manifest.version == "9.0.0" && service.preferences().notified == "9.0.0" && service.preferences().hours == 0 && !service.preferences().balloons, "cached badge and notification dedup survive restart");
            Sleep(1100); require(calls == before, "disabled scheduler makes no requests");
            corrupt = true; service.request(true); awaitState(service, "update.invalid_download");
            require(service.state().staged.empty(), "tampered download never ready");
            response = 403; service.request(); awaitState(service, "update.rate_limit");
            require(!service.state().manifest.version.empty(), "transient rate limiting preserves verified badge");
            const int limited = calls; service.request(); Sleep(100); require(calls == limited, "manual check honors cooldown");
        }
        {
            const int before = calls; UpdateService service; service.start(NULL, config, true); service.request(); Sleep(100);
            require(calls == before && service.state().status == "update.offline", "offline suppresses all update network activity");
        }
        {
            response = 404; UpdateService service; service.start(NULL, config, false); service.request(); awaitState(service, "update.latest");
            require(service.state().manifest.version.empty(), "withdrawn latest release clears cached badge");
        }
        // Admission tests exercise the actual HTTP callback lifetime, including a failed acknowledgement.
        g_cfg.phoneIp = "127.0.0.1";
        HttpRequest request; request.path = "/event"; request.method = "GET"; request.sourceIp = "127.0.0.1"; request.query["ev"] = "est";
        { HttpResponse responseValue; hookHandler(request, responseValue); require(g_pendingCallEvents == 1, "received event reserves admission"); }
        require(g_pendingCallEvents == 0, "failed acknowledgement releases admission reservation");
        { std::lock_guard<std::mutex> guard(g_updateAdmission); g_updateInstalling = true; }
        { HttpResponse responseValue; hookHandler(request, responseValue); require(responseValue.status == 503 && !responseValue.afterSend, "install gate rejects a new established event"); }
        { std::lock_guard<std::mutex> guard(g_updateAdmission); g_updateInstalling = false; }

        // Execute the real helper against owned copies; never replace the running test harness or product.
        wchar_t ownPath[32768]{}; GetModuleFileNameW(NULL, ownPath, 32768); string executableBytes;
        require(updateRead(ownPath, executableBytes, 64 * 1024 * 1024), "read fixture executable");
        const string installManifest = "PhoneRecord-update-v1\nversion=9.0.0\narch=x86\nfile=PhoneRecord.exe\nsize=" + std::to_string(executableBytes.size()) + "\nsha256=" + SigningFixture::digest(executableBytes) + "\n";
        require(SetEnvironmentVariableW(L"PHONERECORD_UPDATE_TEST_PUBLIC_KEY", utf8ToWide(g_updateTestKey).c_str()) != FALSE, "public fixture key environment");
        for (bool failStartup : {false, true}) {
            const wstring directory = folder + (failStartup ? L"\\restore" : L"\\install"), stage = directory + L"\\stage", configPath = directory + L"\\config.ini", target = directory + L"\\PhoneRecord.exe";
            require(CreateDirectoryW(directory.c_str(), NULL) && CreateDirectoryW(stage.c_str(), NULL), "isolated installer workspace");
            require(CopyFileW(ownPath, target.c_str(), TRUE) && CopyFileW(ownPath, (stage + L"\\PhoneRecord.exe").c_str(), TRUE), "owned installer copies");
            require(updateWrite(stage + L"\\update-manifest.txt", installManifest) && updateWrite(stage + L"\\update-manifest.sig", key.sign(installManifest)), "signed installer fixture");
            require(updateWrite(configPath, "synthetic settings\n") && updateWrite(directory + L"\\recording-sentinel.txt", "preserve\n"), "preservation sentinels");
            if (failStartup) require(updateWrite(configPath + L".fail-startup", "fail\n"), "failed startup fixture");
            wstring command = updateArgument(target) + L" --test-install-parent " + updateArgument(directory);
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
            require(CreateProcessW(target.c_str(), &command[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, directory.c_str(), &startup, &process), "launch installer parent");
            require(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0, "parent exits after helper acceptance");
            DWORD exitCode = 1; GetExitCodeProcess(process.hProcess, &exitCode); CloseHandle(process.hThread); CloseHandle(process.hProcess); require(exitCode == 0, "parent prepared installer");
            string result; const ULONGLONG deadline = GetTickCount64() + 15000;
            while (GetTickCount64() < deadline && !updateRead(configPath + L".update-result", result)) Sleep(20);
            require(result == (failStartup ? "update.restored\n" : "update.installed\n"), "real replace/startup/restore result");
            require(GetFileAttributesW((target + L".update-backup").c_str()) == INVALID_FILE_ATTRIBUTES, "backup consumed only after confirmed startup/restore");
            string preserved; require(updateRead(configPath, preserved) && preserved == "synthetic settings\n", "configuration preserved");
            require(updateRead(directory + L"\\recording-sentinel.txt", preserved) && preserved == "preserve\n", "unrelated data preserved");
            Sleep(200);
        }
        SetEnvironmentVariableW(L"PHONERECORD_UPDATE_TEST_PUBLIC_KEY", NULL);
        if (count == 2) { UpdateTrayFixture::run(folder, manifest, signature); AddressAlertFixture::run(folder); }
        for (const wchar_t* suffix : {L".updates", L".update-cache"}) DeleteFileW((config + suffix).c_str());
        puts("UPDATE_NATIVE_OK: versions, signatures, hashes, scheduling, resume, disabled/manual/offline, cache, rate limit, tampering, admission, real replacement and startup rollback");
        require(updateWrite(folder + L"\\native-success.txt", "UPDATE_NATIVE_OK\n"), "success evidence");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "UPDATE_NATIVE_FAILED: %s\n", error.what()); return 1; }
}
