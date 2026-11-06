"""Verify the v2.1 desktop workflows against actual native controls and loopback fixtures.

All contacts and audio are synthetic. Tests address only their owned child windows;
they never use global input, change the clipboard or contact the real phone.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import pathlib
import struct
import tempfile
import time
import wave
from ctypes import wintypes as w

from PIL import Image

from desktop_smoke import USER, capture, dismiss_dialog, title, windows
from product_smoke import App, BASE, FakePhone, http_get, wait_for

#: Native memory operations apply exclusively to the child PID created by this test.
KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
KERNEL.OpenProcess.restype = w.HANDLE
KERNEL.VirtualAllocEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
KERNEL.VirtualAllocEx.restype = ctypes.c_void_p
KERNEL.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
KERNEL.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
KERNEL.VirtualFreeEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD]
KERNEL.CloseHandle.argtypes = [w.HANDLE]
USER.IsIconic.argtypes = [w.HWND]
USER.IsWindowVisible.argtypes = [w.HWND]
USER.GetDpiForWindow.argtypes = [w.HWND]


def cell(pid: int, listing, row: int, column: int) -> str:
    """Read the actual x86 ListView text using its documented target-process buffer contract."""
    process = KERNEL.OpenProcess(0x438, False, pid)
    assert process
    memory = KERNEL.VirtualAllocEx(process, None, 8192, 0x3000, 0x04)
    assert memory and memory < 2**32
    try:
        item = struct.pack("<7I", 1, row, column, 0, 0, memory + 128, 2048)
        source = ctypes.create_string_buffer(item)
        assert KERNEL.WriteProcessMemory(process, memory, source, len(item), None)
        count = USER.SendMessageW(listing, 0x1073, row, memory)
        result = ctypes.create_string_buffer(4096)
        assert KERNEL.ReadProcessMemory(process, memory + 128, result, len(result), None)
        return result.raw[: count * 2].decode("utf-16-le")
    finally:
        KERNEL.VirtualFreeEx(process, memory, 0, 0x8000)
        KERNEL.CloseHandle(process)


def main_window(app: App):
    """Find the exact test child's native owner after its controls have been created."""
    window = wait_for(lambda: next(iter(windows(app.process.pid, "PhoneRecord2Window")), None))
    wait_for(lambda: USER.GetDlgItem(window, 801))
    return window


def health(app: App) -> dict:
    """Read the loopback-only operator health endpoint without phone HTTP traffic."""
    status, body = http_get(app.event, "/health")
    assert status == 200
    return json.loads(body)


def preview(window, output: pathlib.Path, name: str, page: int = 0) -> pathlib.Path:
    """Capture an owned native page using the existing offline-only renderer."""
    USER.SendMessageW(window, 0x0111, 100 + page, 0)
    path = output / ("overview.png" if page == 0 else "recordings.png" if page == 1 else "diagnostics.png" if page == 2 else "settings.png")
    capture(window, path)
    destination = output / f"{name}.png"
    destination.write_bytes(path.read_bytes())
    return destination


def select_row(listing, index: int):
    """Select an owned ListView row using control-local keys without global keyboard input."""
    USER.SendMessageW(listing, 0x0100, 0x24, 0)
    for _ in range(index):
        USER.SendMessageW(listing, 0x0100, 0x28, 0)
    assert USER.SendMessageW(listing, 0x100C, -1, 2) == index


def wav(path: pathlib.Path, seconds: int = 4):
    """Write silent synthetic PCM so playback tests do not emit audible test tones."""
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        output.writeframes(b"\0\0" * 8000 * seconds)


def main() -> int:
    """Exercise metadata, playback, health recovery, scrolling, close preferences and scheduling."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--exe", type=pathlib.Path, default=BASE / "cpp" / "PhoneRecord.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = args.exe.resolve(strict=True)
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-workflow-") as temporary:
        root = pathlib.Path(temporary)
        app = App(root, exe, output, "workflow", gui=True)
        app.args += ["--capture-dir", str(output)]
        folder = app.root / "recordings"
        folder.mkdir()
        names = ["20261103-002000_offline_offline_out_s-1234-abcd.wav",
                 "20261103-001000_offline_offline_out_truncated_s-2345-abcd.wav",
                 "20261103-000500_offline_90001_out_s-3456-abcd.wav"]
        full_name = '测试_联系人, "甲"'
        for name in names:
            wav(folder / name)
        with (folder / "calls.csv").open("w", encoding="utf-8-sig", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["start", "end", "duration", "direction", "local", "remote", "name", "file", "packets"])
            writer.writerow(["synthetic", "synthetic", 4, "out", "offline", "offline", full_name, names[1], 200])
        (app.root / "offline.json").write_text(json.dumps({"90001": "测试联系人😀"}), encoding="utf-8")
        with app:
            window = main_window(app)
            assert not USER.GetDlgItem(window, 500) and not USER.GetDlgItem(window, 501)
            for page in (1, 2, 3, 0):
                path = preview(window, output, f"navigation_{page}", page)
                with Image.open(path) as image:
                    scale = USER.GetDpiForWindow(window) / 96
                    colors = [image.getpixel((round(24 * scale), round((130 + index * 54) * scale)))[:3] for index in range(4)]
                    assert [color == (43, 105, 230) for color in colors] == [index == page for index in range(4)], colors
            checks.append("navigation has one selected menu; overview has no manual recording toggle")

            USER.SendMessageW(window, 0x0111, 101, 0)
            listing = USER.GetDlgItem(window, 610)
            contacts = [cell(app.process.pid, listing, index, 3) for index in range(3)]
            assert contacts == ["offline", full_name + "  offline", "测试联系人😀  90001"], contacts
            checks.append("unnamed outbound contact excludes fingerprint; CSV quotes and JSON Unicode/surrogate pairs preserved")

            select_row(listing, 1)
            USER.PostMessageW(window, 0x0111, 211, 0)
            wait_for(lambda: "PLAYBACK_PLAY" in app.log() or "PLAYBACK_FAILED" in app.log())
            if "PLAYBACK_FAILED" in app.log():
                dismiss_dialog(app.process.pid)
                raise AssertionError("native playback failed; see owned synthetic log")
            time.sleep(.65)
            USER.SendMessageW(window, 0x0111, 216, 0)
            wait_for(lambda: "PLAYBACK_PAUSED" in app.log())
            seek = USER.GetDlgItem(window, 218)
            time.sleep(.55)
            cursor = USER.SendMessageW(seek, 0x0400, 0, 0)
            time.sleep(.6)
            assert USER.SendMessageW(seek, 0x0400, 0, 0) == cursor and cursor > 0
            USER.SendMessageW(seek, 0x0405, True, 850)
            USER.SendMessageW(window, 0x0114, 8, seek)
            USER.SendMessageW(window, 0x0111, 216, 0)
            wait_for(lambda: "PLAYBACK_ENDED" in app.log(), 4)
            USER.SendMessageW(window, 0x0111, 217, 0)
            assert USER.SendMessageW(seek, 0x0400, 0, 0) == 0
            preview(window, output, "internal_player", 1)
            checks.append("silent native WAV playback, pause, resume, seek, natural end and rewind without external player")

            broken = folder / names[0]
            broken.write_bytes(broken.read_bytes()[:44])
            select_row(listing, 0)
            for language, expected in enumerate(("录音文件不完整", "錄音檔不完整", "The recording is incomplete")):
                USER.SendMessageW(USER.GetDlgItem(window, 430), 0x014E, language, 0)
                USER.SendMessageW(window, 0x0111, (1 << 16) | 430, USER.GetDlgItem(window, 430))
                USER.PostMessageW(window, 0x0111, 211, 0)
                dismiss_dialog(app.process.pid, "PLAYBACK_FAILED operation=format")
                assert expected in title(USER.GetDlgItem(window, 219))
                assert USER.GetWindowLongW(USER.GetDlgItem(window, 216), -16) & 0x08000000
            wav(broken)
            before_plays = app.log().count("PLAYBACK_PLAY")
            USER.PostMessageW(window, 0x0111, 211, 0)
            wait_for(lambda: app.log().count("PLAYBACK_PLAY") > before_plays)
            USER.SendMessageW(window, 0x0111, 217, 0)
            checks.append("three-language file errors retain operation details, disable failed sessions and allow repaired-file retry")

            USER.SendMessageW(window, 0x0111, 100, 0)
            for index in range(30):
                assert http_get(app.event, f"/event?ev=synthetic-unknown-{index}")[0] == 400
            time.sleep(.7)
            recent = USER.GetDlgItem(window, 600)
            count = USER.SendMessageW(recent, 0x1004, 0, 0)
            top = USER.SendMessageW(recent, 0x1027, 0, 0)
            per_page = USER.SendMessageW(recent, 0x1028, 0, 0)
            assert top > 0 and top + per_page >= count - 1, (top, per_page, count)
            USER.SendMessageW(USER.GetDlgItem(window, 505), 0x00F1, 0, 0)
            USER.SendMessageW(recent, 0x1013, 0, 0)
            USER.SendMessageW(recent, 0x1014, 0, 600)
            old_top = USER.SendMessageW(recent, 0x1027, 0, 0)
            old_text = cell(app.process.pid, recent, old_top, 0) + cell(app.process.pid, recent, old_top, 2)
            for _ in range(5):
                assert http_get(app.event, "/event?ev=synthetic-unknown-paused")[0] == 400
            time.sleep(.4)
            new_top = USER.SendMessageW(recent, 0x1027, 0, 0)
            new_text = cell(app.process.pid, recent, new_top, 0) + cell(app.process.pid, recent, new_top, 2)
            assert new_text == old_text, (old_top, new_top, old_text, new_text)
            assert all(cell(app.process.pid, recent, index, 1) in ("WARN", "ERROR") for index in range(count))
            checks.append("recent issues exclude routine events, follow the newest issue and preserve the reading position when paused")

            USER.SendMessageW(window, 0x0111, 103, 0)
            USER.SendMessageW(window, 0x0111, 434, 0)
            assert USER.SendMessageW(USER.GetDlgItem(window, 411), 0x0147, 0, 0) == 0
            USER.SendMessageW(USER.GetDlgItem(window, 411), 0x014E, 2, 0)
            USER.SendMessageW(USER.GetDlgItem(window, 412), 0x014E, 0, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid)
            saved = app.config.read_text(encoding="utf-8")
            assert "close_action=exit" in saved and "health_interval=0" in saved
            preview(window, output, "saved_preferences", 3)
            USER.PostMessageW(window, 0x0010, 0, 0)
            assert app.process.wait(5) == 0
            checks.append("saved exit and manual-only check preferences take effect without restarting")

        recovery = App(root, exe, output, "health_recovery", gui=True)
        recovery.args += ["--capture-dir", str(output)]
        blocked = recovery.root / "recordings"
        blocked.write_text("synthetic write blocker", encoding="utf-8")
        with recovery:
            window = main_window(recovery)
            wait_for(lambda: health(recovery)["local_health"] == 2)
            path = preview(window, output, "local_error", 0)
            with Image.open(path) as image:
                scale = USER.GetDpiForWindow(window) / 96
                assert image.getpixel((round(222 * scale), round(363 * scale)))[:3] == (210, 66, 61)
            blocked.unlink()
            USER.SendMessageW(window, 0x0111, 502, 0)
            wait_for(lambda: health(recovery)["local_health"] == 1)
            path = preview(window, output, "local_recovered", 0)
            with Image.open(path) as image:
                scale = USER.GetDpiForWindow(window) / 96
                assert image.getpixel((round(222 * scale), round(363 * scale)))[:3] == (24, 151, 105)
            checks.append("local health error recovers to green independently of previous recording failures")

        for action in ("tray", "minimize"):
            closing = App(root, exe, output, "close_" + action, gui=True)
            with closing.config.open("a", encoding="utf-8") as file:
                file.write(f"close_action={action}\nhealth_interval=0\n")
            with closing:
                window = main_window(closing)
                USER.PostMessageW(window, 0x0010, 0, 0)
                wait_for(lambda: USER.IsIconic(window) if action == "minimize" else not USER.IsWindowVisible(window))
                assert closing.process.poll() is None and health(closing)["control"]
        checks.append("tray and taskbar close modes keep the recorder alive; explicit exit still works")

        with FakePhone("synthetic-only") as phone:
            scheduled = App(root, exe, output, "scheduled_health", gui=True, offline=False, phone_port=phone.port, password=phone.password)
            scheduled.args[scheduled.args.index("--run-for") + 1] = "120"
            with scheduled.config.open("a", encoding="utf-8") as file:
                file.write("health_interval=30\n")
            phone.control_port = scheduled.control
            with scheduled:
                window = main_window(scheduled)
                wait_for(lambda: "AUTH_VERIFIED" in scheduled.log(), 10)
                phone.state = -1
                USER.PostMessageW(window, 0x0111, 503, 0)
                wait_for(lambda: health(scheduled)["phone_health"] == 2)
                phone.state = 4
                USER.PostMessageW(window, 0x0111, 503, 0)
                wait_for(lambda: health(scheduled)["phone_health"] == 1)
                checks.append("phone health failure recovers through a manual successful check")
                assert http_get(scheduled.event, "/event?ev=est")[0] == 200
                wait_for(lambda: phone.media_done.is_set(), 10)
                wait_for(lambda: "START_MEDIA_CONFIRMED" in scheduled.log(), 10)
                before = len(phone.requests)
                deadline = time.monotonic() + 33
                while time.monotonic() < deadline:
                    assert health(scheduled)["call_active"]
                    time.sleep(.5)
                assert len(phone.requests) == before, "background HTTP check spent the active call's budget"
                assert "OPERATION_BEGIN operation=health" not in scheduled.log()
                assert http_get(scheduled.event, "/event?ev=end")[0] == 200
                wait_for(lambda: not health(scheduled)["call_active"] and health(scheduled)["active_sessions"] == 0)
                wait_for(lambda: "OPERATION_END operation=health" in scheduled.log(), 26)
                assert health(scheduled)["local_health"] == health(scheduled)["phone_health"] == 1
                assert phone.start_calls == 1 and not phone.errors
                checks.append("30-second periodic health checks pause for established calls and resume after hangup quiet interval")
    manifest = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
                "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "checks": checks}
    (output / "workflow_results.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
