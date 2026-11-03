"""Verify the native product UI using hidden loopback-only synthetic fixtures.

No global mouse/keyboard input or clipboard changes are performed. Only the
owned child process receives control messages. Captures contain synthetic data.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import pathlib
import secrets
import struct
import sys
import tempfile
import time
import wave
from ctypes import wintypes as w

from PIL import Image

from product_smoke import App, BASE, http_get, wait_for

#: Native APIs are restricted to the child windows created by this test.
USER = ctypes.WinDLL("user32", use_last_error=True)
#: Pointer-sized native message result.
LRESULT = ctypes.c_ssize_t
#: Synchronous enumeration callback type, retained for the duration of each call.
ENUM = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

USER.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
USER.SendMessageW.restype = LRESULT
USER.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
USER.PrintWindow.restype = w.BOOL
USER.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
USER.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
USER.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
USER.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
USER.GetWindowTextLengthW.argtypes = [w.HWND]
USER.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
USER.GetDlgItem.restype = w.HWND
USER.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
USER.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
USER.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
USER.ScreenToClient.argtypes = [w.HWND, ctypes.POINTER(w.POINT)]
USER.GetParent.argtypes = [w.HWND]
USER.GetParent.restype = w.HWND
USER.GetWindowLongW.argtypes = [w.HWND, ctypes.c_int]
USER.GetWindowLongW.restype = ctypes.c_long
USER.EnumWindows.argtypes = [ENUM, w.LPARAM]
USER.EnumChildWindows.argtypes = [w.HWND, ENUM, w.LPARAM]
USER.GetDC.argtypes = [w.HWND]
USER.GetDC.restype = w.HDC
USER.ReleaseDC.argtypes = [w.HWND, w.HDC]

def title(window) -> str:
    """Read standard window text through Windows' supported cross-process marshalling."""
    length = USER.SendMessageW(window, 0x000E, 0, 0)
    buffer = ctypes.create_unicode_buffer(length + 1)
    USER.SendMessageW(window, 0x000D, len(buffer), ctypes.addressof(buffer))
    return buffer.value


def set_text(window, value: str, *, verify: bool = True):
    """Set an owned cross-process EDIT using marshalled WM_SETTEXT, then verify it."""
    buffer = ctypes.create_unicode_buffer(value)
    assert USER.SendMessageW(window, 0x000C, 0, ctypes.addressof(buffer))
    if verify:
        copied = ctypes.create_unicode_buffer(len(value) + 1)
        USER.SendMessageW(window, 0x000D, len(copied), ctypes.addressof(copied))
        assert copied.value == value


def windows(pid: int, class_name: str) -> list:
    """Return only top-level windows owned by the exact test child PID."""
    result = []

    @ENUM
    def visit(window, _):
        """Filter one enumerated window by PID and native class."""
        process = w.DWORD()
        USER.GetWindowThreadProcessId(window, ctypes.byref(process))
        name = ctypes.create_unicode_buffer(128)
        USER.GetClassNameW(window, name, len(name))
        if process.value == pid and name.value == class_name:
            result.append(window)
        return True

    USER.EnumWindows(visit, 0)
    return result


def dismiss_dialog(pid: int, expected: str | None = None):
    """Acknowledge only the owned child's expected validation/save message box."""
    dialog = wait_for(lambda: next(iter(windows(pid, "#32770")), None))
    labels = []
    buttons = []

    @ENUM
    def visit(window, _):
        """Read owned message-box labels without touching password input controls."""
        labels.append(title(window))
        name = ctypes.create_unicode_buffer(128)
        USER.GetClassNameW(window, name, len(name))
        if name.value == "Button":
            buttons.append(window)
        return True

    USER.EnumChildWindows(dialog, visit, 0)
    if expected:
        deadline = time.monotonic() + 3
        while expected not in "\n".join(labels) and time.monotonic() < deadline:
            time.sleep(.03)
            labels.clear()
            buttons.clear()
            USER.EnumChildWindows(dialog, visit, 0)
        assert expected in "\n".join(labels), labels
    # The installed Windows message-box implementation may expose its sole
    # confirmation button as IDOK or IDCANCEL. Use the observed owned control.
    assert len(buttons) == 1, "expected a one-button validation dialog"
    USER.PostMessageW(buttons[0], 0x00F5, 0, 0)
    wait_for(lambda: not windows(pid, "#32770"))


def capture(window, path: pathlib.Path):
    """Request a same-process native render, avoiding foreign-GDI/hidden-window limitations."""
    page = {"overview": 0, "recordings": 1, "diagnostics": 2, "settings": 3}[path.stem]
    bitmap = path.parent / f"PhoneRecord-{page}.bmp"
    USER.SendMessageW(window, 0x0111, 9900, 0)
    assert bitmap.exists(), "native renderer did not produce a bitmap"
    with Image.open(bitmap) as source:
        image = source.convert("RGB")
        assert any(low != high for low, high in image.getextrema()), "blank native render"
        image.save(path)


def decrypt(value: str) -> str:
    """Validate the synthetic saved credential with Windows DPAPI, never printing it."""
    import base64

    class Blob(ctypes.Structure):
        """Native DATA_BLOB for a synthetic credential round trip."""
        _fields_ = [("size", w.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]

    raw = base64.b64decode(value.removeprefix("dpapi:"))
    data = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
    source, output = Blob(len(raw), data), Blob()
    crypt = ctypes.WinDLL("crypt32", use_last_error=True)
    crypt.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, w.DWORD, ctypes.POINTER(Blob)]
    assert crypt.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output))
    kernel = ctypes.WinDLL("kernel32")
    kernel.LocalFree.argtypes = [w.HLOCAL]
    try:
        return ctypes.string_at(output.data, output.size).decode()
    finally:
        kernel.LocalFree(ctypes.cast(output.data, w.HLOCAL))


def main() -> int:
    """Verify native navigation, search, analysis, validation, encrypted save and restart."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if hasattr(USER, "SetProcessDpiAwarenessContext"):
        USER.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        USER.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    exe = BASE / "cpp" / "PhoneRecord.exe"
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-desktop-") as temporary:
        root = pathlib.Path(temporary)
        app = App(root, exe, output, "desktop", gui=True)
        app.args += ["--capture-dir", str(output)]
        recordings = app.root / "recordings"
        recordings.mkdir()
        for index, signal in enumerate(("unknown", "constant")):
            path = recordings / f"20261103-00000{index}_offline_offline_in_synthetic.wav"
            with wave.open(str(path), "wb") as wav:
                wav.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
                wav.writeframes(struct.pack("<h", 5000) * 24000)
            if signal != "unknown":
                pathlib.Path(str(path) + ".quality.json").write_text(json.dumps({"quality": signal, "source": "test_or_other_peer"}), encoding="utf-8")
        with app:
            window = wait_for(lambda: next(iter(windows(app.process.pid, "PhoneRecord2Window")), None))
            wait_for(lambda: USER.GetDlgItem(window, 620))
            USER.PostMessageW(window, 0x0111, 502, 0)
            wait_for(lambda: "LOCAL_CHECK_COMPLETE passed=1" in app.log())
            time.sleep(.6)
            capture(window, output / "overview.png")
            checks.append("overview and local checks")
            USER.SendMessageW(window, 0x0111, 101, 0)
            listing = USER.GetDlgItem(window, 610)
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 2
            set_text(USER.GetDlgItem(window, 210), "missing-fixture")
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 0
            set_text(USER.GetDlgItem(window, 210), "")
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 2
            USER.SendMessageW(listing, 0x0100, 0x24, 0)
            if USER.SendMessageW(listing, 0x100C, -1, 2) < 0:
                USER.SendMessageW(listing, 0x0201, 1, (48 << 16) | 24)
                USER.SendMessageW(listing, 0x0202, 0, (48 << 16) | 24)
            assert USER.SendMessageW(listing, 0x100C, -1, 2) >= 0
            USER.PostMessageW(window, 0x0111, 213, 0)
            wait_for(lambda: "WAV_ANALYSED" in app.log())
            time.sleep(.2)
            capture(window, output / "recordings.png")
            checks.append("recording navigation, search, selection and native audio analysis")
            USER.SendMessageW(window, 0x0111, 102, 0)
            log_list = USER.GetDlgItem(window, 620)
            assert USER.SendMessageW(log_list, 0x1004, 0, 0) > 0
            set_text(USER.GetDlgItem(window, 310), "WAV_ANALYSED")
            assert USER.SendMessageW(log_list, 0x1004, 0, 0) == 1
            USER.SendMessageW(log_list, 0x0100, 0x24, 0)
            detail = USER.GetDlgItem(window, 621)
            buffer = ctypes.create_unicode_buffer(4096)
            USER.SendMessageW(detail, 0x000D, len(buffer), ctypes.addressof(buffer))
            assert "quality=audio" in buffer.value and "目录无法创建" not in buffer.value, "analysis result has misleading error guidance"
            set_text(USER.GetDlgItem(window, 310), "")
            capture(window, output / "diagnostics.png")
            checks.append("diagnostic search and timeline")
            USER.SendMessageW(window, 0x0111, 103, 0)
            before = app.config.read_bytes()
            set_text(USER.GetDlgItem(window, 401), "99999")
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid, "phone_port")
            assert app.config.read_bytes() == before
            set_text(USER.GetDlgItem(window, 401), "9")
            password = secrets.token_urlsafe(16)
            set_text(USER.GetDlgItem(window, 403), password, verify=False)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid)
            saved = app.config.read_text(encoding="utf-8")
            assert "phone_pass=" not in saved and password not in saved
            encrypted = next(line.split("=", 1)[1] for line in saved.splitlines() if line.startswith("phone_pass_dpapi="))
            assert decrypt(encrypted) == password
            assert password not in app.log()
            assert http_get(app.event, "/health")[0] == 200
            capture(window, output / "settings.png")
            checks.append("invalid settings rejected; valid settings atomically saved using user-bound DPAPI")
        reloaded = App(root, exe, output, "encrypted_reload")
        reloaded.config.write_text(saved, encoding="utf-8")
        with reloaded:
            assert "CONFIG_INVALID" not in reloaded.log()
        checks.append("encrypted configuration loads in a fresh native process")
    manifest = {"passed": True, "synthetic_only": True, "real_phone_contacted": False, "checks": checks}
    (output / "desktop_results.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
