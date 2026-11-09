#pragma once
// Included only from the x64 speech page. One-click install of the VB-Audio Virtual Cable that the
// input-method caption path needs; the driver package is downloaded on demand and never stored here.
#ifdef _WIN64

#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")

/// Observable progress of the on-demand virtual cable install.
struct CableInstallState {
    enum Phase { idle, downloading, extracting, installing, verifying, done, failed };
    /// Current step; done and failed are terminal until the user starts again.
    Phase phase = idle;
    /// Stable failure code, empty unless the step failed.
    string failure;
};

/// One process-wide install state read by the 200 ms page refresh.
inline CableInstallState& cableInstall() { static CableInstallState value; return value; }
/// Guard so a second click cannot start a parallel installer.
inline std::atomic<bool>& cableInstalling() { static std::atomic<bool> value{false}; return value; }

/// Report whether a render endpoint named CABLE Input can be opened for playback.
inline bool virtualCableInstalled() {
    for (const auto& device : enumeratePlaybackDevices())
        if (device.name.rfind(L"CABLE Input", 0) == 0) return true;
    return false;
}

/// Private download folder; the package survives a failed install so a retry works offline.
inline wstring cableWorkDir() {
    wchar_t local[32768]{};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local))) return L"";
    const wstring base = wstring(local) + L"\\PhoneRecord";
    const wstring dir = base + L"\\virtual-cable";
    CreateDirectoryW(base.c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

/// Reuse a previously downloaded package instead of fetching it again.
inline wstring cableExistingPackage(const wstring& dir) {
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((dir + L"\\VBCABLE_Driver_Pack*.zip").c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) return L"";
    const wstring path = dir + L"\\" + data.cFileName;
    FindClose(found);
    return path;
}

/// Download the official package, trying the known release names in order.
inline bool cableDownload(const wstring& target) {
    static const wchar_t* urls[] = {
        L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack46.zip",
        L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip",
        L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack44.zip",
    };
    for (const wchar_t* url : urls) {
        if (SUCCEEDED(URLDownloadToFileW(NULL, url, target.c_str(), 0, NULL))) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeHigh == 0 && info.nFileSizeLow > 100000) return true;
        }
    }
    DeleteFileW(target.c_str());
    return false;
}

/// Expand the package with the system tar; returns the extracted folder.
inline bool cableExtract(const wstring& zip, const wstring& dir) {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    wchar_t system[32768]{}; GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    const wstring command = L"\"" + wstring(system) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(NULL, const_cast<wchar_t*>(command.c_str()), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child)) return false;
    WaitForSingleObject(child.hProcess, 120000);
    DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    return code == 0;
}

/// Locate the 64-bit setup program inside the extracted package.
inline wstring cableSetup(const wstring& dir) {
    namespace fs = std::filesystem;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, error)) {
        if (_wcsicmp(entry.path().filename().c_str(), L"VBCABLE_Setup_x64.exe") == 0) return entry.path().wstring();
    }
    return L"";
}

/// Run the signed driver setup elevated; the user approves one UAC prompt.
inline bool cableRunSetup(const wstring& exe, DWORD& result) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = L"-i -h";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) { result = GetLastError(); return false; }
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, 300000);
        GetExitCodeProcess(info.hProcess, &result);
        CloseHandle(info.hProcess);
    }
    return true;
}

/// Download, extract, install and confirm the cable endpoints. Runs on its own thread.
inline void cableInstallThread() {
    CableInstallState& state = cableInstall();
    auto finish = [&](CableInstallState::Phase phase, const char* failure) {
        state.phase = phase; state.failure = failure ? failure : ""; cableInstalling() = false;
    };
    const wstring dir = cableWorkDir();
    if (dir.empty()) { finish(CableInstallState::failed, "permission"); return; }
    wstring zip = cableExistingPackage(dir);
    if (zip.empty()) {
        state.phase = CableInstallState::downloading;
        zip = dir + L"\\VBCABLE_Driver_Pack46.zip";
        if (!cableDownload(zip)) { finish(CableInstallState::failed, "download"); return; }
    }
    state.phase = CableInstallState::extracting;
    const wstring unpacked = dir + L"\\unpacked";
    if (!cableExtract(zip, unpacked)) { finish(CableInstallState::failed, "extract"); return; }
    const wstring setup = cableSetup(unpacked);
    if (setup.empty()) { finish(CableInstallState::failed, "extract"); return; }
    state.phase = CableInstallState::installing;
    DWORD result = 1;
    if (!cableRunSetup(setup, result)) { finish(CableInstallState::failed, result == ERROR_CANCELLED ? "denied" : "setup"); return; }
    if (result != 0) { finish(CableInstallState::failed, "setup"); return; }
    state.phase = CableInstallState::verifying;
    for (int tick = 0; tick < 120; ++tick) {
        if (virtualCableInstalled()) { finish(CableInstallState::done, nullptr); return; }
        Sleep(500);
    }
    finish(CableInstallState::failed, "verify");
}

/// Start one install; ignored while another is running or the cable is already present.
inline void startCableInstall() {
    if (cableInstalling().exchange(true)) return;
    cableInstall().phase = CableInstallState::downloading; cableInstall().failure.clear();
    std::thread(cableInstallThread).detach();
}

#endif
