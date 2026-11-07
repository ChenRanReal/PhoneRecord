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
    DWORD failure = ERROR_INVALID_PARAMETER;
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &length)) { failure = GetLastError(); SetLastError(failure); return false; }
    bool result = false;
    HANDLE handles[] = {inherited, second};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, second ? sizeof(handles) : sizeof(HANDLE), NULL, NULL)) {
        wstring command = updateArgument(exe) + L" " + args;
        result = CreateProcessW(exe.c_str(), &command[0], NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL, &startup.StartupInfo, &process) != FALSE;
        if (!result) failure = GetLastError();
    } else failure = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!result) SetLastError(failure);
    return result;
}

/// Why preparation stopped before the current process was allowed to exit.
enum class UpdatePrepareCode {
    /// Preparation succeeded and the helper accepted the handoff.
    Ok = 0,
    /// Staged executable, manifest or signature did not verify.
    StageInvalid,
    /// The running module path could not be read.
    ModulePath,
    /// PhoneRecord.exe.update-backup still exists beside the program.
    BackupExists,
    /// PhoneRecord.exe.update-new still exists beside the program.
    ReplacementExists,
    /// The helper copy in the download folder could not be created.
    HelperCopy,
    /// The program folder rejected the write probe.
    DirectoryDenied,
    /// The parent process handle could not be inherited.
    ProcessHandle,
    /// The helper acknowledgement event could not be created.
    ReadyEvent,
    /// CreateProcess failed for the helper.
    HelperLaunch,
    /// The helper rejected the staged manifest or executable.
    HelperVerify,
    /// The helper rejected the inherited handoff arguments.
    HelperRejected,
    /// The helper was still running when the acknowledgement timed out.
    HelperTimeout,
    /// The helper exited before acknowledgement with another code.
    HelperFailed
};

/// Concrete preparation failure. Empty when preparation succeeds.
struct UpdatePrepareDetail {
    /// Stable reason used by the UI and tests.
    UpdatePrepareCode code = UpdatePrepareCode::Ok;
    /// Win32 code for the failing local call, or zero when the helper decided.
    DWORD win32 = 0;
    /// Helper process exit code when it quit before acknowledgement.
    DWORD helperExit = 0;
    /// Leftover or unwritable file the operator can act on.
    wstring blocking;
};

/// Stable ASCII name for logs. Not a localized message.
static const char* updatePrepareName(UpdatePrepareCode code) {
    switch (code) {
    case UpdatePrepareCode::Ok: return "ok";
    case UpdatePrepareCode::StageInvalid: return "stage";
    case UpdatePrepareCode::ModulePath: return "module";
    case UpdatePrepareCode::BackupExists: return "backup";
    case UpdatePrepareCode::ReplacementExists: return "replacement";
    case UpdatePrepareCode::HelperCopy: return "helper_copy";
    case UpdatePrepareCode::DirectoryDenied: return "directory";
    case UpdatePrepareCode::ProcessHandle: return "process";
    case UpdatePrepareCode::ReadyEvent: return "event";
    case UpdatePrepareCode::HelperLaunch: return "launch";
    case UpdatePrepareCode::HelperVerify: return "verify";
    case UpdatePrepareCode::HelperRejected: return "rejected";
    case UpdatePrepareCode::HelperTimeout: return "timeout";
    case UpdatePrepareCode::HelperFailed: return "helper_exit";
    }
    return "helper_exit";
}

/// True when the path is one of the two installer leftovers that may be kept aside.
static bool updateLeftoverPath(const wstring& path) {
    const wchar_t* suffixes[] = {L".update-backup", L".update-new"};
    for (const wchar_t* suffix : suffixes) {
        const size_t length = wcslen(suffix);
        if (path.size() >= length && _wcsicmp(path.c_str() + path.size() - length, suffix) == 0) return true;
    }
    return false;
}

/// Rename one leftover installer file to the same name plus .kept. Never overwrites an existing kept file.
static bool updateKeepAside(const wstring& path, wstring& kept, DWORD& error) {
    kept.clear();
    if (!updateLeftoverPath(path)) { error = ERROR_INVALID_NAME; return false; }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { error = ERROR_FILE_NOT_FOUND; return false; }
    kept = path + L".kept";
    if (GetFileAttributesW(kept.c_str()) != INVALID_FILE_ATTRIBUTES) { error = ERROR_ALREADY_EXISTS; kept.clear(); return false; }
    if (!MoveFileExW(path.c_str(), kept.c_str(), MOVEFILE_WRITE_THROUGH)) { error = GetLastError(); kept.clear(); return false; }
    error = 0; return true;
}

/// Validate the stage again, then prepare a helper which waits on this exact process handle.
/// On failure, detail receives the blocking reason. The current process keeps running.
static bool updateBeginInstall(const UpdateState& state, const wstring& config, UpdatePrepareDetail* detail = nullptr) {
    auto fail = [&](UpdatePrepareCode code, DWORD win32, const wstring& blocking = L"", DWORD helperExit = 0) {
        if (detail) { detail->code = code; detail->win32 = win32; detail->blocking = blocking; detail->helperExit = helperExit; }
        return false;
    };
    if (detail) *detail = {};
    if (state.staged.empty() || !updateSignature(state.signedManifest, state.signature) || !updateVerifyFile(state.staged, state.manifest)) return fail(UpdatePrepareCode::StageInvalid, 0);
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(NULL, executable, static_cast<DWORD>(std::size(executable)))) return fail(UpdatePrepareCode::ModulePath, GetLastError());
    const wstring backup = wstring(executable) + L".update-backup", replacement = wstring(executable) + L".update-new";
    // Refuse before exiting: a leftover backup otherwise makes the helper exit without acknowledging.
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) return fail(UpdatePrepareCode::BackupExists, 0, backup);
    if (GetFileAttributesW(replacement.c_str()) != INVALID_FILE_ATTRIBUTES) return fail(UpdatePrepareCode::ReplacementExists, 0, replacement);
    const wstring helper = state.staged.substr(0, state.staged.find_last_of(L"\\/")) + L"\\PhoneRecord-updater.exe";
    if (!CopyFileW(executable, helper.c_str(), TRUE)) {
        DWORD copyError = GetLastError();
        // A crashed preparation can leave only this helper copy. Remove that copy once and retry.
        const bool replaced = copyError == ERROR_FILE_EXISTS && DeleteFileW(helper.c_str()) && CopyFileW(executable, helper.c_str(), TRUE);
        if (!replaced) {
            if (copyError == ERROR_FILE_EXISTS) copyError = GetLastError();
            return fail(UpdatePrepareCode::HelperCopy, copyError, helper);
        }
    }
    // Check write access before the main process exits; install failure must leave it usable.
    const wstring probe = wstring(executable) + L".update-probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) { const DWORD probeError = GetLastError(); DeleteFileW(helper.c_str()); return fail(UpdatePrepareCode::DirectoryDenied, probeError, probe); }
    CloseHandle(file);
    HANDLE parent = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) { const DWORD handleError = GetLastError(); DeleteFileW(helper.c_str()); return fail(UpdatePrepareCode::ProcessHandle, handleError); }
    SECURITY_ATTRIBUTES security{sizeof(security), NULL, TRUE};
    HANDLE ready = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!ready) { const DWORD eventError = GetLastError(); CloseHandle(parent); DeleteFileW(helper.c_str()); return fail(UpdatePrepareCode::ReadyEvent, eventError); }
    PROCESS_INFORMATION process{};
    const wstring args = L"--apply-update " + std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(ready)) + L" " + updateArgument(executable) + L" " + updateArgument(state.staged) + L" " + updateArgument(config) + L" PhoneRecord-update-v1";
    const bool launched = updateLaunch(helper, args, parent, process, ready);
    const DWORD launchError = GetLastError();
    CloseHandle(parent);
    bool accepted = false;
    UpdatePrepareCode helperCode = UpdatePrepareCode::HelperFailed; DWORD helperExit = 0; wstring helperBlocking;
    if (launched) {
        HANDLE handles[] = {ready, process.hProcess};
        accepted = WaitForMultipleObjects(2, handles, FALSE, 10000) == WAIT_OBJECT_0;
        if (!accepted) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 22); WaitForSingleObject(process.hProcess, 5000); helperCode = UpdatePrepareCode::HelperTimeout;
            } else {
                GetExitCodeProcess(process.hProcess, &helperExit);
                if (helperExit == 21) helperCode = UpdatePrepareCode::HelperVerify;
                else if (helperExit == 20) helperCode = UpdatePrepareCode::HelperRejected;
                else if (helperExit == 23) {
                    helperCode = GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES ? UpdatePrepareCode::BackupExists : UpdatePrepareCode::ReplacementExists;
                    helperBlocking = helperCode == UpdatePrepareCode::BackupExists ? backup : replacement;
                    helperExit = 0;
                }
            }
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    else DeleteFileW(helper.c_str());
    CloseHandle(ready);
    if (!accepted) DeleteFileW(helper.c_str());
    if (!accepted) return fail(launched ? helperCode : UpdatePrepareCode::HelperLaunch, launched ? 0 : launchError, helperBlocking, helperExit);
    return true;
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
        if (!DeleteFileW(backup.c_str()) && GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const wstring kept = backup + L".kept";
            if (GetFileAttributesW(kept.c_str()) == INVALID_FILE_ATTRIBUTES) MoveFileExW(backup.c_str(), kept.c_str(), MOVEFILE_WRITE_THROUGH);
        }
        updateWrite(resultPath, "update.installed\n");
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
