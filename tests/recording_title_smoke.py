"""Verify native historical-call titles using only owned windows and synthetic recordings."""

from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import pathlib
import stat
import tempfile
from ctypes import wintypes as w

from desktop_smoke import USER, set_text, title, windows
from desktop_workflow import cell, main_window, select_row, wav
from localization_smoke import capture_page, select_language
from product_smoke import App, BASE, wait_for


def editor(app: App, window, *, f2: bool = False):
    """Open the real modal editor and hide only the owned test dialog from the desktop."""
    if f2:
        USER.SendMessageW(USER.GetDlgItem(window, 610), 0x0100, 0x71, 0)
    else:
        USER.PostMessageW(window, 0x0111, 221, 0)
    def created():
        """Identify the editor by its stable input ID rather than a localized title."""
        return next((dialog for dialog in windows(app.process.pid, "#32770") if USER.GetDlgItem(dialog, 1101)), None)
    dialog = wait_for(created)
    USER.ShowWindow(dialog, 0)
    return dialog


def close_editor(dialog, command: int = 2):
    """Complete or cancel the owned modal dialog without changing global keyboard input."""
    USER.PostMessageW(dialog, 0x0111, command, 0)
    wait_for(lambda: not USER.IsWindow(dialog))


def save_title(app: App, window, value: str):
    """Save through the actual edit control and native persistence handler."""
    dialog = editor(app, window)
    set_text(USER.GetDlgItem(dialog, 1101), value, verify=False)
    assert title(USER.GetDlgItem(dialog, 1101)) == value
    close_editor(dialog, 1)


def dismiss_notice(app: App, containing: str, parent=None):
    """Dismiss an expected nested validation message while retaining its editor for correction."""
    dialog = wait_for(lambda: next((item for item in windows(app.process.pid, "#32770")
                                   if item != parent and not USER.GetDlgItem(item, 1101)), None))
    labels = []
    buttons = []
    from desktop_smoke import ENUM
    @ENUM
    def visit(child, _):
        """Inspect only labels/buttons belonging to the exact synthetic message box."""
        labels.append(title(child))
        kind = ctypes.create_unicode_buffer(64)
        USER.GetClassNameW(child, kind, len(kind))
        if kind.value == "Button":
            buttons.append(child)
        return True
    USER.EnumChildWindows(dialog, visit, 0)
    assert containing in "\n".join(labels), labels
    assert len(buttons) == 1
    USER.PostMessageW(buttons[0], 0x00F5, 0, 0)
    wait_for(lambda: not USER.IsWindow(dialog))


def preserved(paths: list[pathlib.Path]) -> dict:
    """Compare file contents and modification times without exposing call metadata in evidence."""
    return {str(path): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns) for path in paths}


def minimum_layout(window):
    """Check the complete two-row recording toolbar at native-DPI minimum window size."""
    USER.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
    USER.SetThreadDpiAwarenessContext.restype = w.HANDLE
    previous = USER.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    assert previous
    try:
        USER.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
        dpi = USER.GetDpiForWindow(window)
        area = w.RECT()
        USER.GetWindowRect(window, ctypes.byref(area))
        assert USER.SetWindowPos(window, None, area.left, area.top, (980 * dpi + 95) // 96, (760 * dpi + 95) // 96, 0x0014)
        gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        gdi.SelectObject.argtypes = [w.HDC, w.HANDLE]
        gdi.SelectObject.restype = w.HANDLE
        gdi.GetTextExtentPoint32W.argtypes = [w.HDC, w.LPCWSTR, ctypes.c_int, ctypes.POINTER(w.SIZE)]
        positions = []
        for control in (211, 221, 212, 213, 214, 215):
            child = USER.GetDlgItem(window, control)
            bounds, extent = w.RECT(), w.SIZE()
            USER.GetWindowRect(child, ctypes.byref(bounds))
            dc = USER.GetDC(child)
            font = gdi.SelectObject(dc, USER.SendMessageW(child, 0x0031, 0, 0))
            try:
                label = title(child)
                assert gdi.GetTextExtentPoint32W(dc, label, len(label), ctypes.byref(extent))
                assert extent.cx + 28 * dpi // 96 <= bounds.right - bounds.left, label
            finally:
                gdi.SelectObject(dc, font)
                USER.ReleaseDC(child, dc)
            positions.append(bounds)
        assert all(left.right < right.left for left, right in zip(positions, positions[1:]))
        search, listing = w.RECT(), w.RECT()
        USER.GetWindowRect(USER.GetDlgItem(window, 210), ctypes.byref(search))
        USER.GetWindowRect(USER.GetDlgItem(window, 610), ctypes.byref(listing))
        assert search.bottom < positions[0].top and positions[-1].bottom < listing.top
        assert positions[-1].right <= listing.right
    finally:
        USER.SetThreadDpiAwarenessContext(previous)


def main() -> int:
    """Exercise rename, cancellation, validation, stale/error safety, persistence, search and reset."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord title 測試 ") as temporary:
        app = App(pathlib.Path(temporary), BASE / "cpp/PhoneRecord.exe", output, "titles", gui=True)
        app.args[app.args.index("--run-for") + 1] = "180"
        app.args += ["--capture-dir", str(output)]
        directory = app.root / "recordings"
        directory.mkdir()
        paths = [directory / f"20261104-10000{index}_6000_700{index}_in_synthetic.wav" for index in (1, 2)]
        originals = []
        with (directory / "calls.csv").open("w", encoding="utf-8", newline="") as ledger:
            writer = csv.writer(ledger)
            for index, path in enumerate(paths, 1):
                wav(path)
                quality = pathlib.Path(str(path) + ".quality.json")
                quality.write_text(json.dumps({"quality": "audio", "source": "test_or_other_peer", "display_name": f"Original {index}"}), encoding="utf-8")
                writer.writerow(["2026-11-04", "10:00:0" + str(index), "4", "in", "6000", "700" + str(index), f"Original {index}", path.name, path.stat().st_size])
                originals += [path, quality]
        originals.append(directory / "calls.csv")
        original = preserved(originals)
        sidecar = pathlib.Path(str(paths[1]) + ".title.json")
        with app:
            window = main_window(app)
            select_language(window, 2)
            USER.SendMessageW(window, 0x0111, 101, 0)
            listing = USER.GetDlgItem(window, 610)
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 2
            assert not USER.IsWindowEnabled(USER.GetDlgItem(window, 221))
            select_row(listing, 0)
            assert USER.IsWindowEnabled(USER.GetDlgItem(window, 221))
            assert cell(app.process.pid, listing, 0, 1) == "Original 2"
            contact = cell(app.process.pid, listing, 0, 3)
            checks.append("default title and original contact remain separate; rename requires a selected stable recording")

            custom = '  客戶回訪 "A/B:*?<>|" 😀  '
            save_title(app, window, custom)
            expected = custom.strip()
            assert json.loads(sidecar.read_text(encoding="utf-8")) == {"title": expected}
            assert cell(app.process.pid, listing, 0, 1) == expected
            assert cell(app.process.pid, listing, 0, 3) == contact and preserved(originals) == original
            USER.SendMessageW(window, 0x0111, 211, 0)
            assert title(USER.GetDlgItem(window, 219)) == expected
            USER.SendMessageW(window, 0x0111, 217, 0)
            checks.append("Unicode/emoji/quotes and filename-reserved characters persist as a display title; original files, timestamps, call facts and playback stay intact")

            saved = sidecar.read_bytes()
            dialog = editor(app, window, f2=True)
            USER.SendMessageW(dialog, 0x0111, 1105, 0)
            assert title(USER.GetDlgItem(dialog, 1101)) == ""
            close_editor(dialog)
            assert sidecar.read_bytes() == saved
            dialog = editor(app, window)
            set_text(USER.GetDlgItem(dialog, 1101), "x" * 121)
            USER.PostMessageW(dialog, 0x0111, 1, 0)
            dismiss_notice(app, "120", dialog)
            assert USER.IsWindow(dialog) and sidecar.read_bytes() == saved
            close_editor(dialog)
            checks.append("F2 opens the editor; reset is a draft until Save; Cancel and over-limit input preserve the prior title")

            sidecar.chmod(stat.S_IREAD)
            try:
                dialog = editor(app, window)
                set_text(USER.GetDlgItem(dialog, 1101), "cannot replace read-only metadata")
                USER.PostMessageW(dialog, 0x0111, 1, 0)
                dismiss_notice(app, "Cannot save", dialog)
                assert sidecar.read_bytes() == saved and not list(directory.glob("*.pending-*"))
                close_editor(dialog)
            finally:
                sidecar.chmod(stat.S_IWRITE | stat.S_IREAD)
            dialog = editor(app, window)
            sidecar.write_text('{"title":"External edit"}\n', encoding="utf-8")
            USER.PostMessageW(dialog, 0x0111, 1, 0)
            dismiss_notice(app, "Another operation", dialog)
            close_editor(dialog)
            assert json.loads(sidecar.read_text(encoding="utf-8"))["title"] == "External edit"
            checks.append("read-only destination and stale dialogs cannot overwrite prior metadata; failed commits clean up their own temporary file")

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
            kernel.CreateFileW.restype = w.HANDLE
            kernel.CloseHandle.argtypes = [w.HANDLE]
            locked = kernel.CreateFileW(str(paths[1]), 0x40000000, 3, None, 3, 0x80, None)
            assert locked != ctypes.c_void_p(-1).value
            try:
                dialog = editor(app, window)
                USER.PostMessageW(dialog, 0x0111, 1, 0)
                dismiss_notice(app, "being written", dialog)
                close_editor(dialog)
            finally:
                kernel.CloseHandle(locked)
            invalid = b'{"title":"broken"}\x00'
            sidecar.write_bytes(invalid)
            USER.PostMessageW(window, 0x0111, 221, 0)
            dismiss_notice(app, "Cannot read")
            assert sidecar.read_bytes() == invalid
            sidecar.write_bytes(saved)
            USER.SendMessageW(window, 0x0111, 214, 0)
            checks.append("active recording writers and malformed title files are rejected without touching audio or damaged metadata")

            for language, caption, save in [(0, "重命名通话", "保存名称"), (1, "重新命名通話", "保存名稱"), (2, "Rename call", "Save title")]:
                select_language(window, language)
                USER.SendMessageW(window, 0x0111, 101, 0)
                select_row(listing, 0)
                minimum_layout(window)
                dialog = editor(app, window)
                assert title(dialog) == caption and title(USER.GetDlgItem(dialog, 1)) == save
                assert "120" in title(USER.GetDlgItem(dialog, 1104))
                close_editor(dialog)
                capture_page(window, output, f"rename_language_{language}", 1)
            checks.append("all three native editors and the complete recording toolbar fit the minimum window at the real monitor DPI")

        with app:
            window = main_window(app)
            USER.SendMessageW(window, 0x0111, 101, 0)
            listing = USER.GetDlgItem(window, 610)
            set_text(USER.GetDlgItem(window, 210), "客戶回訪")
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 1 and cell(app.process.pid, listing, 0, 1) == expected
            set_text(USER.GetDlgItem(window, 210), "7002")
            assert USER.SendMessageW(listing, 0x1004, 0, 0) == 1
            select_row(listing, 0)
            save_title(app, window, "😀" * 120)
            assert json.loads(sidecar.read_text(encoding="utf-8"))["title"] == "😀" * 120
            checks.append("saved names survive a new process and match search alongside original numbers; the limit counts Unicode scalars rather than UTF-16 halves")
            dialog = editor(app, window)
            USER.SendMessageW(dialog, 0x0111, 1105, 0)
            close_editor(dialog, 1)
            assert not sidecar.exists() and cell(app.process.pid, listing, 0, 1) == "Original 2"
            assert preserved(originals) == original
            assert expected not in app.log() and "😀" not in app.log()
            dialog = editor(app, window)
            set_text(USER.GetDlgItem(dialog, 1101), "uncommitted shutdown draft")
        assert not sidecar.exists() and not windows(app.process.pid, "#32770")
        assert preserved(originals) == original
        checks.append("Restore default removes only the title sidecar; clean shutdown cancels an open draft; original history is retained and logs exclude title text")
    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
              "exe_sha256": hashlib.sha256((BASE / "cpp/PhoneRecord.exe").read_bytes()).hexdigest(), "checks": checks}
    (output / "title_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
