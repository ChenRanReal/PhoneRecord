"""Exercise first use, quiet notifications and read-only compatibility inspection.

All phone pages and audio are synthetic loopback fixtures. No global clipboard,
mouse or keyboard state is touched, and no real phone is contacted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
import time

from desktop_smoke import USER, dismiss_dialog, set_text, title
from desktop_workflow import main_window
from localization_smoke import capture_page, select_help, select_language
from product_smoke import App, BASE, FakePhone, http_get, startup_info, wait_for


def inspection(app: App) -> dict:
    """Read only the product's loopback report, which excludes addresses and callback values."""
    status, body = http_get(app.event, "/compatibility")
    assert status == 200
    report = json.loads(body)
    assert report["addresses_hidden"] and "127.0.0.1" not in body and "198.51.100.7" not in body
    return report


def run_check(window, app: App) -> dict:
    """Invoke the real native command and wait for a new completed inspection snapshot."""
    previous = inspection(app)["revision"]
    USER.SendMessageW(window, 0x0111, 804, 0)
    def completed():
        """Observe a completed generation after the native command has been dispatched."""
        report = inspection(app)
        return report if report["verdict"] != "running" and report["revision"] != previous else None
    return wait_for(completed, 12)


def main() -> int:
    """Cover the supported profile, settings failures, insufficient evidence and fresh-user path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = BASE / "cpp/PhoneRecord.exe"
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-onboarding-") as temporary:
        root = pathlib.Path(temporary)
        with FakePhone("synthetic-onboarding") as phone:
            app = App(root, exe, output, "compatibility", gui=True, offline=False, phone_port=phone.port, password=phone.password)
            app.args[app.args.index("--run-for") + 1] = "180"
            phone.control_port, phone.callback_port = app.control, app.event
            with app:
                window = main_window(app)
                wait_for(lambda: "AUTH_VERIFIED" in app.log(), 10)
                baseline = len(phone.requests)
                report = run_check(window, app)
                assert report["verdict"] == "ready" and report["recognized_profile"]
                assert all(item["status"] == "pass" for item in report["checks"] if item["key"] not in ("runtime", "speech"))
                assert [item[1:] for item in phone.requests[baseline:]] == [("GET", "/record.htm"), ("GET", "/action.htm")]
                assert phone.start_calls == 0 and not report["runtime_verified"]
                select_language(window, 2)
                body = select_help(window, 5)
                assert "Ready for call acceptance" in body and "two-way" in body.lower() and "127.0.0.1" not in body
                checks.append("matching profile/settings use GET only and leave real-call/speech acceptance pending")

                cases = [("codec", "G722", "codec"), ("enabled", False, "enabled"), ("mode", "2", "mode"),
                         ("server", "198.51.100.7", "address"), ("control", app.control + 1, "control_port")]
                for setting, invalid, key in cases:
                    previous = phone.compatibility[setting]
                    phone.compatibility[setting] = invalid
                    report = run_check(window, app)
                    assert report["verdict"] == "settings" and any(item["key"] == key and item["status"] == "fail" for item in report["checks"])
                    assert "198.51.100.7" not in select_help(window, 5)
                    phone.compatibility[setting] = previous
                phone.callback_port += 1
                report = run_check(window, app)
                assert report["verdict"] == "settings" and sum(item["status"] == "fail" for item in report["checks"]) == 2
                phone.callback_port -= 1
                checks.append("disabled recording, wrong mode/codec/server/control port and mismatched callbacks produce targeted fixes without exposing values")

                phone.compatibility["schema"] = False
                assert run_check(window, app)["verdict"] == "unsupported"
                phone.compatibility["schema"] = True
                phone.compatibility["actions"] = "missing"
                assert run_check(window, app)["verdict"] == "unsupported"
                phone.compatibility["actions"] = "login"
                assert run_check(window, app)["verdict"] == "unavailable"
                phone.compatibility["actions"] = "normal"
                assert phone.start_calls == 0
                checks.append("unknown page schema and login disguise remain unsupported/undetermined rather than falsely compatible")

                set_text(USER.GetDlgItem(window, 400), "198.51.100.7")
                before = len(phone.requests)
                assert run_check(window, app)["verdict"] == "save_required"
                assert len(phone.requests) == before
                set_text(USER.GetDlgItem(window, 400), "127.0.0.1")
                assert run_check(window, app)["verdict"] == "ready"
                assert http_get(app.event, "/event?ev=est")[0] == 200
                try:
                    wait_for(lambda: phone.media_done.is_set(), 12)
                    wait_for(lambda: "START_MEDIA_CONFIRMED" in app.log(), 10)
                    before = len(phone.requests)
                    assert run_check(window, app)["verdict"] == "deferred"
                    assert len(phone.requests) == before
                finally:
                    assert http_get(app.event, "/event?ev=end")[0] == 200
                    wait_for(lambda: "STOP_COMPLETE" in app.log(), 8)
                    wait_for(lambda: "WAV_CLOSED" in app.log() and json.loads(http_get(app.event, "/health")[1])["active_sessions"] == 0, 8)
                assert not inspection(app)["runtime_verified"]
                checks.append("unsaved settings and active calls prevent inspection traffic; synthetic RTP cannot satisfy real-phone evidence")

        quiet = App(root, exe, output, "quiet", gui=True)
        quiet.args += ["--capture-dir", str(output)]
        with quiet:
            window = main_window(quiet)
            USER.SendMessageW(window, 0x0111, 103, 0)
            assert USER.SendMessageW(USER.GetDlgItem(window, 414), 0x00F0, 0, 0) == 0
            USER.SendMessageW(window, 0x0010, 0, 0)
            wait_for(lambda: "TRAY_NOTIFICATION_SUPPRESSED preference=0" in quiet.log())
            USER.SendMessageW(USER.GetDlgItem(window, 414), 0x00F1, 1, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(quiet.process.pid)
            assert "tray_notifications=1" in quiet.config.read_text(encoding="utf-8")
            USER.SendMessageW(window, 0x0010, 0, 0)
            wait_for(lambda: "TRAY_NOTIFICATION_SUPPRESSED preference=1 offline=1" in quiet.log())
            assert "TRAY_NOTIFICATION_REQUESTED" not in quiet.log()
            saved = quiet.config.read_bytes()
            capture_page(window, output, "notification_settings", 3)
            select_language(window, 1)
            select_help(window, 6)
            capture_page(window, output, "first_use_guide", 4)
        loaded = App(root, exe, output, "notification_reload", gui=True)
        loaded.config.write_bytes(saved)
        with loaded:
            window = main_window(loaded)
            assert USER.SendMessageW(USER.GetDlgItem(window, 414), 0x00F0, 0, 0) == 1
        checks.append("quiet default suppresses balloons; user setting applies immediately, persists and reloads; offline tests never notify the desktop")

        first = App(root, exe, output, "first_use", gui=True)
        first.args += ["--capture-dir", str(output)]
        first.config.unlink()
        first.process = subprocess.Popen(first.args, cwd=first.root, startupinfo=startup_info(), creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            window = main_window(first)
            assert all(title(USER.GetDlgItem(window, control)) == "" for control in (400, 402, 403, 404))
            wait_for(lambda: USER.GetWindowLongW(USER.GetDlgItem(window, 400), -16) & 0x10000000)
            assert not USER.GetWindowLongW(USER.GetDlgItem(window, 600), -16) & 0x10000000
            USER.SendMessageW(window, 0x0111, 503, 0)
            wait_for(lambda: "COMMAND_REQUIRES_VALID_CONFIG" in first.log())
            assert "HTTP_SEND" not in first.log()
            select_language(window, 1)
            USER.SendMessageW(window, 0x0111, 422, 0)
            body = title(USER.GetDlgItem(window, 801))
            assert "<PC_IP>" in body and "<EVENT_PORT>" in body and "20" in body and "雙方" in body
        finally:
            first.shutdown()
        checks.append("exe-only first launch opens blank required settings, blocks guessed network requests and supplies a complete bilingual first-setup flow")

    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
              "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "checks": checks}
    (output / "onboarding_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
