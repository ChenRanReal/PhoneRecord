"""Verify actual Shell GUID registration across owned process and binary replacement."""

import argparse
import ctypes
import hashlib
import pathlib
import shutil
import tempfile
from ctypes import wintypes as w

from desktop_smoke import USER
from desktop_workflow import main_window
from product_smoke import App, wait_for


class IconIdentifier(ctypes.Structure):
    """Native NOTIFYICONIDENTIFIER layout used to query Explorer's live icon."""

    _fields_ = [("cbSize", w.DWORD), ("hWnd", w.HWND), ("uID", w.UINT),
                ("guidItem", ctypes.c_ubyte * 16)]


class IconData(ctypes.Structure):
    """Full Windows 7+ NOTIFYICONDATAW layout for removing only an owned icon."""

    _fields_ = [("cbSize", w.DWORD), ("hWnd", w.HWND), ("uID", w.UINT),
                ("uFlags", w.UINT), ("callback", w.UINT), ("hIcon", w.HICON),
                ("tip", w.WCHAR * 128), ("state", w.DWORD), ("stateMask", w.DWORD),
                ("info", w.WCHAR * 256), ("version", w.UINT),
                ("infoTitle", w.WCHAR * 64), ("infoFlags", w.DWORD),
                ("guidItem", ctypes.c_ubyte * 16), ("balloonIcon", w.HICON)]


def identity(exe):
    """Compute the documented path-scoped UUID independently of the executable."""
    data = ("PhoneRecord.notification-area.v1\0" + str(exe).lower()).encode("utf-16-le")
    digest = bytearray(hashlib.sha256(data).digest()[:16])
    digest[7] = (digest[7] & 0x0f) | 0x80
    digest[8] = (digest[8] & 0x3f) | 0x80
    icon = IconIdentifier()
    icon.cbSize = ctypes.sizeof(icon)
    icon.guidItem[:] = digest
    return icon


def main():
    """Replace only an isolated test binary and query its GUID before/after restart."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    shell = ctypes.WinDLL("shell32")
    shell.Shell_NotifyIconGetRect.argtypes = [ctypes.POINTER(IconIdentifier), ctypes.POINTER(w.RECT)]
    shell.Shell_NotifyIconGetRect.restype = ctypes.c_long
    shell.Shell_NotifyIconW.argtypes = [w.DWORD, ctypes.POINTER(IconData)]
    USER.RegisterWindowMessageW.argtypes = [w.LPCWSTR]
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="PhoneRecord tray 測試 ") as temporary:
        root = pathlib.Path(temporary)
        exe = root / "PhoneRecord.exe"
        shutil.copyfile(args.exe.resolve(), exe)
        icon = identity(exe)

        def registered():
            """Query the live shell by GUID only, without a process window handle."""
            return shell.Shell_NotifyIconGetRect(ctypes.byref(icon), ctypes.byref(w.RECT())) == 0

        app = App(root, exe, args.output.resolve(), "tray_identity", gui=True)
        for generation in range(2):
            with app:
                window = main_window(app)
                wait_for(registered)
                USER.SendMessageW(window, 0x0111, 105, 0)
                wait_for(registered)
                # Remove only this owned icon to simulate Explorer losing its icon table.
                removed = IconData()
                removed.cbSize = ctypes.sizeof(removed)
                removed.uFlags = 0x20  # NIF_GUID
                removed.guidItem[:] = icon.guidItem
                assert shell.Shell_NotifyIconW(2, ctypes.byref(removed))  # NIM_DELETE
                wait_for(lambda: not registered())
                USER.SendMessageW(window, USER.RegisterWindowMessageW("TaskbarCreated"), 0, 0)
                wait_for(registered)
                USER.SendMessageW(window, 0x0010, 0, 0)
                assert not USER.IsWindowVisible(window)
                USER.SendMessageW(window, 0x0111, 904, 0)
                assert USER.IsWindowVisible(window)
            wait_for(lambda: not registered())
            if generation == 0:
                replacement = root / "replacement.exe"
                shutil.copyfile(exe, replacement)
                with replacement.open("ab") as stream:
                    stream.write(b"tray-update-test-overlay")
                replacement.replace(exe)
        print("PASS: GUID registration, icon refresh, shell recreation message, tray restore, cleanup and binary replacement")


if __name__ == "__main__":
    main()
