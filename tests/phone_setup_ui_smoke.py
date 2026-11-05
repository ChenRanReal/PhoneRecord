"""Verify in-app setup using owned native windows and a loopback-only phone."""

import argparse
import copy
import ctypes
import hashlib
import json
from pathlib import Path
import tempfile

from desktop_smoke import USER, ENUM, title, windows, set_text, dismiss_dialog
from desktop_workflow import main_window
from localization_smoke import select_language, select_help, resources, capture_page
from phone_setup_smoke import Phone
from product_smoke import App, wait_for, start_frame, http_get


def dialog_text(dialog):
    """Read only the native dialog belonging to the exact test process."""
    labels = []

    @ENUM
    def visit(child, _):
        """Collect the child dialog labels using marshalled window text messages."""
        labels.append(title(child))
        return True

    USER.EnumChildWindows(dialog, visit, 0)
    return "\n".join(labels)


def respond(app, expected, button):
    """Confirm or cancel one expected product dialog without global input."""
    dialog = wait_for(lambda: next(iter(windows(app.process.pid, "#32770")), None), 15)
    text = wait_for(lambda: dialog_text(dialog) if expected in dialog_text(dialog) else None, 4)
    assert "[Missing text]" not in text
    control = USER.GetDlgItem(dialog, button)
    if not control and button == 1:
        control = USER.GetDlgItem(dialog, 2)  # Native one-button message boxes may expose IDCANCEL.
    assert control, (expected, button, text)
    USER.PostMessageW(control, 0x00F5, 0, 0)
    # A rejection dialog may replace the confirmation immediately, reusing its HWND.
    wait_for(lambda: dialog not in windows(app.process.pid, "#32770") or
             expected not in dialog_text(dialog))
    return text


def begin(window):
    """Invoke the real Settings button command, rather than a testing-only API."""
    USER.SendMessageW(window, 0x0111, 103, 0)
    USER.PostMessageW(window, 0x0111, 424, 0)


def main():
    """Cover review, cancellation, stale plans, live calls, writes and rollback."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    resources()
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-direct-setup-") as temporary:
        with Phone() as phone:
            app = App(Path(temporary), exe, output, "direct", gui=True, offline=False,
                      phone_port=phone.port, password="synthetic-setup")
            app.args[app.args.index("--run-for") + 1] = "240"
            # Reproduce the colleague's actual category: only the two callbacks differ.
            phone.record.update(EnableRecord_RW="1", CSE_RecordType_RW="1", CSE_RecordVoiceCodec_R="PCMA",
                                SYS_RecordAddr_RW="127.0.0.1", SYS_RecordPort_RW=str(app.control))
            with app:
                window = main_window(app)
                wait_for(lambda: "OPERATION_END operation=compatibility" in app.log(), 12)
                for language, label in enumerate(("一键设置话机", "一鍵設定話機", "Configure phone")):
                    select_language(window, language)
                    assert title(USER.GetDlgItem(window, 424)) == label
                    help_text = select_help(window, 6)
                    assert label in help_text and "<EVENT_PORT>" in help_text
                checks.append("three-language button and first-use help explain direct configuration and callback repair")

                begin(window)
                preview = respond(app, "Review changes", 7)  # IDNO
                assert "Call Established" in preview and "Call Terminated" in preview
                assert "Recording mode:" not in preview and not phone.writes
                checks.append("matching recording settings with two missing callbacks preview only the required changes; cancel never writes")

                begin(window)
                dialog = wait_for(lambda: next(iter(windows(app.process.pid, "#32770")), None))
                wait_for(lambda: "Review changes" in dialog_text(dialog))
                phone.actions["KeepOther"] += " changed after preview"
                respond(app, "Review changes", 6)
                respond(app, "plan changed", 1)
                assert not phone.writes
                checks.append("changing the phone after review rejects the stale plan without writes")

                begin(window)
                dialog = wait_for(lambda: next(iter(windows(app.process.pid, "#32770")), None))
                wait_for(lambda: "Review changes" in dialog_text(dialog))
                assert app.request(start_frame("setup-active-call"))["result"] == "success"
                respond(app, "Review changes", 6)
                respond(app, "End calls", 1)
                app.stop_session("setup-active-call")
                assert not phone.writes
                checks.append("an incoming control session after preview prevents applying settings")

                # Wait until session cleanup releases its active-session count.
                wait_for(lambda: json.loads(http_get(app.event, '/health')[1])["active_sessions"] == 0, 5)
                begin(window)
                respond(app, "Review changes", 6)
                respond(app, "Settings were applied and verified", 1)
                assert phone.writes == ["/action.htm"]
                assert phone.actions["AURL_CallEstablished_RW"] == f"http://127.0.0.1:{app.event}/event?ev=est"
                assert phone.actions["AURL_CallTerminated_RW"] == f"http://127.0.0.1:{app.event}/event?ev=end"
                assert list((app.root / "phone-setup-backups").glob("*.dpapi"))
                assert app.process.poll() is None
                wait_for(lambda: "COMPATIBILITY_COMPLETE" in app.log() or "OPERATION_END operation=compatibility" in app.log())
                checks.append("confirmation repairs only Action URLs with encrypted backup and readback; app remains running")

                count = len(phone.writes)
                begin(window)
                respond(app, "Settings already match", 1)
                assert len(phone.writes) == count
                checks.append("already-matching settings skip writing")

                before = copy.deepcopy(phone.actions)
                phone.actions["AURL_CallEstablished_RW"] = "https://example.test/company-action"
                begin(window)
                respond(app, "non-recording callback", 1)
                assert len(phone.writes) == count
                phone.actions = before
                checks.append("unrelated company callback conflicts stop before any write")

                phone.actions["AURL_CallEstablished_RW"] = ""
                before = copy.deepcopy(phone.actions)
                phone.corrupt_once = True
                begin(window)
                respond(app, "Review changes", 6)
                respond(app, "original settings were restored", 1)
                assert phone.actions == before
                checks.append("unexpected readback differences trigger verified restoration in the same running app")

                phone.deny = True
                begin(window)
                respond(app, "Review changes", 6)
                respond(app, "cannot change settings", 1)
                assert phone.actions == before
                phone.deny = False
                checks.append("web write denial reports permission guidance without claiming success")

                USER.SendMessageW(window, 0x0111, 103, 0)
                set_text(USER.GetDlgItem(window, 404), "127.0.0.2")
                USER.PostMessageW(window, 0x0111, 424, 0)
                respond(app, "Save settings", 1)
                set_text(USER.GetDlgItem(window, 404), "127.0.0.1")
                assert not any(path in ("/Start", "/Stop") for _, path in phone.requests)
                checks.append("unsaved connection drafts block setup; setup never sends Start/Stop")

        # Offline screenshots contain synthetic settings only, with no phone requests.
        offline = App(Path(temporary), exe, output, "direct_layout", gui=True)
        offline.args += ["--capture-dir", str(output)]
        with offline:
            window = main_window(offline)
            for language in range(3):
                select_language(window, language)
                capture_page(window, output, f"direct_settings_{language}", 3)

    manifest = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
                "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "checks": checks}
    (output / "direct_setup_results.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
