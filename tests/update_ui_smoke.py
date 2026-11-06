"""Exercise the real update page and tray message paths using owned offline windows only."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import pathlib
import tempfile

from ctypes import wintypes as w
from desktop_smoke import USER, set_text, title
from desktop_workflow import main_window
from localization_smoke import capture_page, resources, select_language
from product_smoke import App, BASE, wait_for


def select_interval(window, index):
    """Invoke an update preference menu command without touching phone settings."""
    USER.SendMessageW(window, 0x0111, 840 + index, 0)


def verify_layout(window):
    """Check visible update actions and notes stay inside the minimum native client bounds."""
    USER.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    USER.SetWindowPos(window, None, 0, 0, 980, 760, 0x0016)
    client = w.RECT()
    USER.GetClientRect(window, ctypes.byref(client))
    positions = []
    for control in (820, 821, 822, 826, 827, 829, 830):
        child = USER.GetDlgItem(window, control)
        if not USER.GetWindowLongW(child, -16) & 0x10000000:
            continue
        area = w.RECT()
        USER.GetWindowRect(child, ctypes.byref(area))
        point = w.POINT(area.left, area.top)
        USER.ScreenToClient(window, ctypes.byref(point))
        assert point.x >= 0 and point.y >= 0
        assert point.x + area.right - area.left <= client.right + 1
        assert point.y + area.bottom - area.top <= client.bottom + 1
        if control in (820, 821, 822):
            positions.append(area)
    assert all(left.right < right.left for left, right in zip(positions, positions[1:]))


def main():
    """Verify translations, immediate independent preferences, hidden-window updates and shell recreation."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--exe", type=pathlib.Path, default=BASE / "cpp/PhoneRecord.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    count = resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord update UI 測試 ") as temporary:
        app = App(pathlib.Path(temporary), args.exe.resolve(), output, "update_ui", gui=True)
        app.args += ["--capture-dir", str(output)]
        preferences = pathlib.Path(str(app.config) + ".updates")
        original = app.config.read_bytes()
        with app:
            window = main_window(app)
            for language, label in [(0, "软件更新"), (1, "軟體更新"), (2, "Updates")]:
                select_language(window, language)
                USER.SendMessageW(window, 0x0111, 105, 0)
                assert title(USER.GetDlgItem(window, 105)) == label
                assert USER.SendMessageW(USER.GetDlgItem(window, 824), 0x0147, 0, 0) == 2
                assert USER.SendMessageW(USER.GetDlgItem(window, 825), 0x00F0, 0, 0) == 1
                assert not USER.IsWindowEnabled(USER.GetDlgItem(window, 821))
                assert not USER.IsWindowEnabled(USER.GetDlgItem(window, 822))
                verify_layout(window)
                capture_page(window, output, f"updates_language_{language}", 5)
            checks.append("three translated update pages render at minimum window size; download/install remain disabled offline")
            # Keep a connection draft open while changing independent update preferences.
            USER.SendMessageW(window, 0x0111, 103, 0)
            set_text(USER.GetDlgItem(window, 402), "unsaved synthetic draft")
            USER.SendMessageW(window, 0x0111, 105, 0)
            select_interval(window, 0)
            USER.SendMessageW(window, 0x0111, 845, 0)
            assert preferences.read_text(encoding="utf-8") == "hours=0\nballoons=0\nnotified=\n"
            assert app.config.read_bytes() == original
            assert title(USER.GetDlgItem(window, 402)) == "unsaved synthetic draft"
            checks.append("update settings save immediately without validating or overwriting unsaved phone drafts")
            USER.SendMessageW(window, 0x0010, 0, 0)
            assert not USER.IsWindowVisible(window)
            USER.SendMessageW(window, 0x0111, 903, 0)
            assert "Offline" in title(USER.GetDlgItem(window, 826))
            USER.RegisterWindowMessageW.argtypes = [w.LPCWSTR]
            shell_message = USER.RegisterWindowMessageW("TaskbarCreated")
            USER.SendMessageW(window, shell_message, 0, 0)
            USER.SendMessageW(window, 0x0111, 904, 0)
            assert USER.IsWindowVisible(window)
            assert USER.IsWindowVisible(USER.GetDlgItem(window, 827))
            checks.append("hidden tray window processes checks and shell recreation; tray update action restores the update page")
        with app:
            window = main_window(app)
            USER.SendMessageW(window, 0x0111, 105, 0)
            assert USER.SendMessageW(USER.GetDlgItem(window, 824), 0x0147, 0, 0) == 0
            assert USER.SendMessageW(USER.GetDlgItem(window, 825), 0x00F0, 0, 0) == 0
            select_interval(window, 4)
            assert "hours=24" in preferences.read_text(encoding="utf-8")
            assert app.config.read_bytes() == original
            checks.append("disabled periodic checks and update balloons survive restart; the 24-hour option saves independently")
    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
              "resources_per_language": count, "checks": checks,
              "exe_sha256": hashlib.sha256(args.exe.resolve().read_bytes()).hexdigest()}
    (output / "update_ui_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
