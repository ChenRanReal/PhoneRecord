"""Verify current-user startup registration and hidden recording with synthetic local fixtures.

The native harness redirects HKCU into its own disposable namespace. The GUI
check temporarily changes only PhoneRecord's actual Run value and restores it
in finally. No other startup value, real phone, clipboard or input is changed.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import winreg
from ctypes import wintypes as w

from desktop_smoke import USER, set_text, title
from desktop_workflow import main_window
from localization_smoke import capture_page, select_language
from product_smoke import App, BASE, emit_rtp, http_get, start_frame, startup_info, wait_for

RUN = r"Software\Microsoft\Windows\CurrentVersion\Run"
VALUE = "PhoneRecord"
VIEW = winreg.KEY_WOW64_64KEY


def startup_values() -> dict:
    """Snapshot values for preservation checks without emitting any command contents."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN, 0, winreg.KEY_READ | VIEW) as key:
            values = {}
            for index in range(winreg.QueryInfoKey(key)[1]):
                name, value, kind = winreg.EnumValue(key, index)
                values[name] = (value, kind)
            return values
    except FileNotFoundError:
        return {}


def replace_startup(value: tuple | None):
    """Change only this product's native-view value, including exact restoration of a prior type."""
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN, 0, winreg.KEY_SET_VALUE | VIEW) as key:
        if value is None:
            try:
                winreg.DeleteValue(key, VALUE)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(key, VALUE, 0, value[1], value[0])


def command_arguments(command: str) -> list[str]:
    """Parse the stored command with Windows itself, including Unicode and quoted paths."""
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [w.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(w.LPWSTR)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    count = ctypes.c_int()
    values = shell.CommandLineToArgvW(command, ctypes.byref(count))
    assert values
    try:
        return [values[index] for index in range(count.value)]
    finally:
        kernel.LocalFree(values)


def toggle(window, enabled: bool):
    """Dispatch a real native checkbox change only to this test's owned window."""
    USER.SendMessageW(USER.GetDlgItem(window, 415), 0x00F1, int(enabled), 0)
    USER.SendMessageW(window, 0x0111, 415, 0)
    assert USER.SendMessageW(USER.GetDlgItem(window, 415), 0x00F0, 0, 0) == int(enabled)


def verify_preference_layout(window):
    """Measure the owned app in physical pixels without caller-side DPI virtualization."""
    USER.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
    USER.SetThreadDpiAwarenessContext.restype = w.HANDLE
    previous = USER.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    assert previous
    try:
        verify_layout_at_native_dpi(window)
    finally:
        USER.SetThreadDpiAwarenessContext(previous)


def verify_layout_at_native_dpi(window):
    """Keep both complete labels readable and separate at the supported minimum window size."""
    USER.GetDpiForWindow.argtypes = [w.HWND]
    USER.GetDpiForWindow.restype = w.UINT
    USER.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    USER.GetDC.argtypes = [w.HWND]
    USER.GetDC.restype = w.HDC
    USER.ReleaseDC.argtypes = [w.HWND, w.HDC]
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    gdi.SelectObject.argtypes = [w.HDC, w.HANDLE]
    gdi.SelectObject.restype = w.HANDLE
    gdi.GetTextExtentPoint32W.argtypes = [w.HDC, w.LPCWSTR, ctypes.c_int, ctypes.POINTER(w.SIZE)]
    dpi = USER.GetDpiForWindow(window)
    parent = w.RECT()
    USER.GetWindowRect(window, ctypes.byref(parent))
    assert USER.SetWindowPos(window, None, parent.left, parent.top, (980 * dpi + 95) // 96, (760 * dpi + 95) // 96, 0x0014)
    bounds = []
    for control in (414, 415):
        child = USER.GetDlgItem(window, control)
        area, extent = w.RECT(), w.SIZE()
        USER.GetWindowRect(child, ctypes.byref(area))
        dc = USER.GetDC(child)
        font = gdi.SelectObject(dc, USER.SendMessageW(child, 0x0031, 0, 0))
        try:
            label = title(child)
            assert gdi.GetTextExtentPoint32W(dc, label, len(label), ctypes.byref(extent))
            assert extent.cx + (24 * dpi + 95) // 96 <= area.right - area.left, label
        finally:
            gdi.SelectObject(dc, font)
            USER.ReleaseDC(child, dc)
        bounds.append(area)
    assert bounds[0].right < bounds[1].left and bounds[0].top == bounds[1].top
    save = w.RECT()
    USER.GetWindowRect(USER.GetDlgItem(window, 420), ctypes.byref(save))
    assert max(area.bottom for area in bounds) < save.top


def native_harness(root: pathlib.Path) -> dict:
    """Compile and run isolated Windows API checks using the product's existing MSVC toolchain."""
    msvc = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231")
    sdk = pathlib.Path(r"C:\Program Files (x86)\Windows Kits\10")
    version = "10.0.26100.0"
    environment = os.environ.copy()
    environment["PATH"] = str(msvc / "bin/Hostx64/x86") + os.pathsep + environment["PATH"]
    environment["INCLUDE"] = ";".join(map(str, [msvc / "include", *(sdk / "Include" / version / part for part in ("ucrt", "um", "shared"))]))
    environment["LIB"] = ";".join(map(str, [msvc / "lib/x86", sdk / "Lib" / version / "ucrt/x86", sdk / "Lib" / version / "um/x86"]))
    executable = root / "startup-harness.exe"
    compiled = subprocess.run([str(msvc / "bin/Hostx64/x86/cl.exe"), "/nologo", "/std:c++17", "/MT", "/EHsc", "/utf-8",
                               "/DUNICODE", "/D_UNICODE", "/D_WIN32_WINNT=0x0601", str(BASE / "tests/user_startup_harness.cpp"),
                               "/Fe:" + str(executable), "/Fo:" + str(root / "startup-harness.obj"), "/link", "advapi32.lib"],
                              env=environment, cwd=root, capture_output=True, startupinfo=startup_info(),
                              creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
    assert compiled.returncode == 0, compiled.stdout.decode(errors="replace") + compiled.stderr.decode(errors="replace")
    ran = subprocess.run([str(executable)], capture_output=True, startupinfo=startup_info(),
                         creationflags=subprocess.CREATE_NO_WINDOW, timeout=15)
    assert ran.returncode == 0, ran.stderr.decode(errors="replace")
    return json.loads(ran.stdout)


def main() -> int:
    """Verify actual registration, opt-out, localization, portability and background services, then restore Windows."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = startup_values()
    previous = original.get(VALUE)
    neighbors = {key: value for key, value in original.items() if key != VALUE}
    checks = []
    try:
        with tempfile.TemporaryDirectory(prefix="PhoneRecord startup 測試 %!&'-") as temporary:
            root = pathlib.Path(temporary)
            native = native_harness(root)
            assert native["passed"] and native["checks"] == 8
            replace_startup(None)
            portable = root / "portable bin"
            portable.mkdir()
            executable = portable / "PhoneRecord.exe"
            shutil.copyfile(BASE / "cpp/PhoneRecord.exe", executable)
            app = App(root, executable, output, "own config", gui=True)
            app.args += ["--capture-dir", str(output)]
            before = app.config.read_bytes()
            manual_arguments = list(app.args)
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 103, 0)
                assert USER.SendMessageW(USER.GetDlgItem(window, 415), 0x00F0, 0, 0) == 0
                assert VALUE not in startup_values()
                checks.append("first launch reads Windows and does not register startup automatically")

                set_text(USER.GetDlgItem(window, 400), "")
                toggle(window, True)
                command, kind = startup_values()[VALUE]
                arguments = command_arguments(command)
                assert kind == winreg.REG_SZ and arguments == [str(executable), "--config", str(app.config), "--startup"]
                assert app.config.read_bytes() == before and "AUTOSTART_CHANGED enabled=1 verified=1" in app.log()
                assert {key: value for key, value in startup_values().items() if key != VALUE} == neighbors
                toggle(window, True)
                assert startup_values()[VALUE] == (command, kind)
                checks.append("enable immediately binds quoted Unicode executable/configuration paths, preserves drafts and unrelated values, and is idempotent")

                expected = ["开机启动（后台）", "開機啟動（背景）", "Start at sign-in (background)"]
                for language, label in enumerate(expected):
                    select_language(window, language)
                    USER.SendMessageW(window, 0x0111, 103, 0)
                    assert title(USER.GetDlgItem(window, 415)) == label
                    USER.SendMessageW(window, 0x0111, (6 << 16) | 415, 0)
                    assert title(USER.GetDlgItem(window, 722)) and "[Missing text]" not in title(USER.GetDlgItem(window, 722))
                    verify_preference_layout(window)
                    capture_page(window, output, f"startup_language_{language}", 3)
                    assert startup_values()[VALUE] == (command, kind)
                checks.append("all three languages translate the live startup control and its field/status guidance without rewriting registration")

            assert startup_values()[VALUE] == (command, kind), "Exit removed the user's startup preference"
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 103, 0)
                assert USER.SendMessageW(USER.GetDlgItem(window, 415), 0x00F0, 0, 0) == 1
                replace_startup(('"C:\\another copy\\PhoneRecord.exe" --startup', winreg.REG_SZ))
                USER.SendMessageW(window, 0x0111, 103, 0)
                USER.SendMessageW(window, 0x0111, (6 << 16) | 415, 0)
                assert "Another copy" in title(USER.GetDlgItem(window, 722))
                set_text(USER.GetDlgItem(window, 400), "")
                toggle(window, False)
                assert VALUE not in startup_values() and app.config.read_bytes() == before
                toggle(window, True)
                assert startup_values()[VALUE] == (command, kind)
            checks.append("a new process and external registration changes are reflected; another copy can be removed or transferred independently of invalid drafts")

            app.args = arguments + ["--offline", "--run-for", "60", "--export-report", str(app.root / "report.txt")]
            with app:
                window = main_window(app)
                wait_for(lambda: "AUTOSTART_WINDOW hidden=1 tray_available=1 config_valid=1" in app.log())
                assert not USER.GetWindowLongW(window, -16) & 0x10000000
                assert http_get(app.event, "/health")[0] == 200
                session = "startup-synthetic"
                reply = app.request(start_frame(session))
                emit_rtp(int(reply["port"]), [0x80, 0xa0, 0x90] * 50)
                app.request(("cmd=RecordStopRequest&id=" + session).encode())
                wait_for(lambda: "WAV_CLOSED" in app.log())
                assert list((app.root / "recordings").glob("*.wav"))
                duplicate = subprocess.run(arguments + ["--offline"], startupinfo=startup_info(),
                                           creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
                assert duplicate.returncode == 3 and app.process.poll() is None
                assert not USER.GetWindowLongW(window, -16) & 0x10000000
                toggle(window, False)
                assert VALUE not in startup_values()
            checks.append("registered startup runs in an available tray, records synthetic audio while hidden, and exits a duplicate invocation without a popup")

            app.args = manual_arguments
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 103, 0)
                assert USER.SendMessageW(USER.GetDlgItem(window, 415), 0x00F0, 0, 0) == 0 and VALUE not in startup_values()
                assert app.config.read_bytes() == before
            checks.append("opt-out persists across process restarts without a settings-file flag or automatic re-registration")
    finally:
        replace_startup(previous)
    assert startup_values().get(VALUE) == previous
    assert {key: value for key, value in startup_values().items() if key != VALUE} == neighbors
    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False, "windows_startup_restored": True,
              "exe_sha256": hashlib.sha256((BASE / "cpp/PhoneRecord.exe").read_bytes()).hexdigest(),
              "isolated_native_checks": native["checks"], "checks": checks}
    (output / "startup_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
