#pragma once

#include "update_protocol.hpp"

/// Quote a Windows argv element without interpreting shell metacharacters.
static wstring updateArgument(const wstring& value) {
    wstring result = L"\""; size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\'); else result.append(slashes, L'\\');
        slashes = 0; result += ch;
    }
    result.append(slashes * 2, L'\\'); return result + L'\"';
}

/// Launch exactly one inherited handle using the Windows attribute list, with no shell.
static bool updateLaunch(const wstring& exe, const wstring& args, HANDLE inherited, PROCESS_INFORMATION& process, HANDLE second = NULL) {
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    SIZE_T length = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &length);
    std::vector<BYTE> storage(length); startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &length)) return false;
    bool result = false;
    HANDLE handles[] = {inherited, second};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, second ? sizeof(handles) : sizeof(HANDLE), NULL, NULL)) {
        wstring command = updateArgument(exe) + L" " + args;
        result = CreateProcessW(exe.c_str(), &command[0], NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL, &startup.StartupInfo, &process) != FALSE;
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList); return result;
}

/// Validate the stage again, then prepare a helper which waits on this exact process handle.
static bool updateBeginInstall(const UpdateState& state, const wstring& config) {
    if (state.staged.empty() || !updateSignature(state.signedManifest, state.signature) || !updateVerifyFile(state.staged, state.manifest)) return false;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(NULL, executable, static_cast<DWORD>(std::size(executable)))) return false;
    const wstring helper = state.staged.substr(0, state.staged.find_last_of(L"\\/")) + L"\\PhoneRecord-updater.exe";
    if (!CopyFileW(executable, helper.c_str(), TRUE)) return false;
    // Check write access before the main process exits; install failure must leave it usable.
    const wstring probe = wstring(executable) + L".update-probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(helper.c_str()); return false; }
    CloseHandle(file);
    HANDLE parent = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) { DeleteFileW(helper.c_str()); return false; }
    SECURITY_ATTRIBUTES security{sizeof(security), NULL, TRUE};
    HANDLE ready = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!ready) { CloseHandle(parent); DeleteFileW(helper.c_str()); return false; }
    PROCESS_INFORMATION process{};
    const wstring args = L"--apply-update " + std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(ready)) + L" " + updateArgument(executable) + L" " + updateArgument(state.staged) + L" " + updateArgument(config) + L" PhoneRecord-update-v1";
    const bool launched = updateLaunch(helper, args, parent, process, ready);
    CloseHandle(parent);
    bool accepted = false;
    if (launched) {
        HANDLE handles[] = {ready, process.hProcess};
        accepted = WaitForMultipleObjects(2, handles, FALSE, 10000) == WAIT_OBJECT_0;
        if (!accepted && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) { TerminateProcess(process.hProcess, 22); WaitForSingleObject(process.hProcess, 5000); }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    else DeleteFileW(helper.c_str());
    CloseHandle(ready);
    if (!accepted) DeleteFileW(helper.c_str());
    return accepted;
}

/// Replace only the program, confirm native startup through an inherited event, and restore on failure.
static int updateApply(int count, LPWSTR* args) {
    if (count != 8) return 20;
    wchar_t* end = NULL; const uint64_t number = _wcstoui64(args[2], &end, 10);
    if (!number || !end || *end) return 20;
    HANDLE parent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number)); DWORD flags = 0;
    if (!GetHandleInformation(parent, &flags)) return 20;
    const uint64_t readyNumber = _wcstoui64(args[3], &end, 10);
    if (!readyNumber || !end || *end) { CloseHandle(parent); return 20; }
    HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(readyNumber));
    if (!GetHandleInformation(ready, &flags)) { CloseHandle(parent); return 20; }
    const wstring target = args[4], staged = args[5], config = args[6];
    // Only an inherited exact-parent handle may authorize replacement of its program.
    if (wcscmp(args[7], L"PhoneRecord-update-v1") != 0) { CloseHandle(parent); return 20; }
    wchar_t parentPath[32768]{}; DWORD length = static_cast<DWORD>(std::size(parentPath));
    if (!QueryFullProcessImageNameW(parent, 0, parentPath, &length) || _wcsicmp(parentPath, target.c_str())) { CloseHandle(parent); return 20; }
    const wstring directory = staged.substr(0, staged.find_last_of(L"\\/"));
    string bytes, signature; UpdateManifest manifest;
    if (!updateRead(directory + L"\\update-manifest.txt", bytes) || !updateRead(directory + L"\\update-manifest.sig", signature, 8192) ||
        !updateSignature(bytes, signature) || !manifest.parse(bytes) || !updateNewer(manifest.version) || !updateVerifyFile(staged, manifest)) { CloseHandle(parent); return 21; }
    if (GetFileAttributesW((target + L".update-backup").c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW((target + L".update-new").c_str()) != INVALID_FILE_ATTRIBUTES) { CloseHandle(ready); CloseHandle(parent); return 23; }
    SetEvent(ready); CloseHandle(ready);
    const DWORD wait = WaitForSingleObject(parent, 60000); CloseHandle(parent);
    if (wait != WAIT_OBJECT_0) return 22;
    const wstring replacement = target + L".update-new", backup = target + L".update-backup";
    const wstring resultPath = config + L".update-result";
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) { updateWrite(resultPath, "update.backup_exists\n"); return 23; }
    bool installed = CopyFileW(staged.c_str(), replacement.c_str(), TRUE) && updateVerifyFile(replacement, manifest) &&
        ReplaceFileW(target.c_str(), replacement.c_str(), backup.c_str(), 0, NULL, NULL);
    const auto restart = [&](bool confirm) {
        SECURITY_ATTRIBUTES security{sizeof(security), NULL, TRUE};
        HANDLE ack = CreateEventW(&security, TRUE, FALSE, NULL); if (!ack) return false;
        PROCESS_INFORMATION process{};
        wstring command = L"--config " + updateArgument(config) + L" --startup";
        if (confirm) command += L" --update-ack " + std::to_wstring(reinterpret_cast<uintptr_t>(ack));
        const bool launched = updateLaunch(target, command, ack, process);
        bool ready = launched;
        if (launched) {
            if (confirm) {
                HANDLE handles[] = {ack, process.hProcess};
                ready = WaitForMultipleObjects(2, handles, FALSE, 60000) == WAIT_OBJECT_0;
                if (!ready && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                    // Only the unacknowledged child launched by this helper may be terminated.
                    TerminateProcess(process.hProcess, 24); WaitForSingleObject(process.hProcess, 5000);
                }
            }
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        CloseHandle(ack); return ready;
    };
    if (installed && restart(true)) {
        DeleteFileW(backup.c_str()); updateWrite(resultPath, "update.installed\n");
        DeleteFileW(staged.c_str()); DeleteFileW((directory + L"\\update-manifest.txt").c_str()); DeleteFileW((directory + L"\\update-manifest.sig").c_str());
        updateWrite(config + L".update-cleanup", wideToUtf8(directory));
        return 0;
    }
    DeleteFileW(replacement.c_str());
    if (installed || GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const bool restored = GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES ? MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE : ReplaceFileW(target.c_str(), backup.c_str(), NULL, 0, NULL, NULL) != FALSE;
        if (!restored) { updateWrite(resultPath, "update.restore_failed\n"); return 25; }
        updateWrite(resultPath, "update.restored\n");
    } else updateWrite(resultPath, "update.install_failed\n");
    updateWrite(config + L".update-cleanup", wideToUtf8(directory));
    restart(false); return 24;
}
