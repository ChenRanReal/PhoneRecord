"""Exercise native phone setup against a loopback-only, stateful firmware fixture."""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import html
import json
import pathlib
import re
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch
from urllib.parse import parse_qs

from product_smoke import App, BASE, startup_info


class Phone:
    """Model two persistent forms, independent company callbacks and bounded failures."""

    def __init__(self):
        """Create a fixture with disabled recording and no recording callbacks."""
        self.record = {"EnableRecord_RW": "0", "OtherToggle_RW": "1", "CSE_RecordType_RW": "2",
                       "CSE_RecordVoiceCodec_R": "G722", "SYS_RecordAddr_RW": "192.0.2.10", "SYS_RecordPort_RW": "9999",
                       "KeepText": '保留 "&<> value', "CheckBoxManager": "EnableRecord_RW,OtherToggle_RW,HiddenToggle_RW"}
        self.actions = {"AURL_CallEstablished_RW": "", "AURL_CallTerminated_RW": "",
                        "AURL_Registered_RW": "https://example.test/status?token=synthetic-only", "KeepOther": "保持不變"}
        self.requests = []
        self.writes = []
        self.corrupt_once = False
        self.deny = False
        self.status = "4"
        self.schema = True
        owner = self

        class Handler(BaseHTTPRequestHandler):
            """Serve the actual native client's challenge and complete form workflow."""

            def log_message(self, *_):
                """Keep request contents and fixture credentials out of console output."""

            def answer(self, body, status=200, cookie=False):
                """Return a bounded HTTP response and close the connection."""
                data = body.encode("utf-8")
                self.send_response(status)
                if cookie:
                    self.send_header("Set-Cookie", "auth=0123456789abcdef; path=/")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                """Expose selected options, checkbox wire values and CRLF-wrapped inputs."""
                owner.requests.append(("GET", self.path.split("?")[0]))
                if self.path == "/":
                    self.answer("logon_content", cookie=True)
                elif self.path.startswith("/key==nonce"):
                    self.answer("0123456789abcdef")
                elif self.path == "/record.htm":
                    fields = dict(owner.record)
                    if not owner.schema:
                        fields.pop("CheckBoxManager")
                    body = f'<script>var recordStatus="{owner.status}";</script><form method="post">'
                    for key, value in fields.items():
                        if key in ("EnableRecord_RW", "OtherToggle_RW"):
                            wire = "ON" if key == "EnableRecord_RW" else "YES"
                            body += f'<input type="checkbox" name="{key}" value="{wire}" ' + ('checked' if value == "1" else '') + '>'
                        elif key in ("CSE_RecordType_RW", "CSE_RecordVoiceCodec_R"):
                            options = ("1", "2") if key == "CSE_RecordType_RW" else ("PCMA", "PCMU", "G722")
                            body += f'<select name="{key}">' + ''.join(f'<option value="{option}" ' + ('selected' if option == value else '') + f'>{option}</option>' for option in options) + '</select>'
                        else:
                            body += f'<input name="{key}" value="\r\n{html.escape(value, quote=True)}\r\n">'
                    self.answer(body + '</form>')
                elif self.path == "/action.htm":
                    self.answer('<form method="post">' + ''.join(f'<input name="{key}" value="\r\n{html.escape(value, quote=True)}\r\n">' for key, value in owner.actions.items()) + '</form>')
                else:
                    self.answer("missing", 404)

            def do_POST(self):
                """Apply the submitted form and emulate a one-time unexpected persisted change."""
                owner.requests.append(("POST", self.path))
                data = parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("ascii"), keep_blank_values=True)
                values = {key: value[0] for key, value in data.items()}
                if self.path == "/":
                    expected = hashlib.md5(b"offline:synthetic-setup:0123456789abcdef").hexdigest()
                    self.answer("OK" if values.get("encoded") == "offline:" + expected else "Password Error")
                    return
                if self.path not in ("/record.htm", "/action.htm"):
                    self.answer("forbidden", 403)
                    return
                owner.writes.append(self.path)
                assert values.get("DefaultSubmit") == "Apply" and values.get("ReturnPage") == self.path
                if owner.deny:
                    self.answer("Not Authorized", 403)
                    return
                target = owner.record if self.path == "/record.htm" else owner.actions
                for key in values.get("CheckBoxManager", "").split(","):
                    if key in target:
                        target[key] = "1" if key in values else "0"
                for key, value in values.items():
                    if key in target and key not in ("EnableRecord_RW", "OtherToggle_RW", "CheckBoxManager"):
                        target[key] = value
                if self.path == "/action.htm" and owner.corrupt_once:
                    owner.actions["KeepOther"] = "unexpected firmware change"
                    owner.corrupt_once = False
                self.answer("OK")

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        """Start the owned loopback fixture."""
        self.thread.start()
        return self

    def __exit__(self, *_):
        """Join the fixture without touching any real phone."""
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def command(app, *arguments, expected=0):
    """Run only a native setup command and retain its address-safe status for assertions."""
    report = app.root / "phone-setup-report.json"
    report.unlink(missing_ok=True)
    result = subprocess.run([str(app.exe), "--config", str(app.config), "--setup-report", str(report), *arguments],
                            startupinfo=startup_info(), creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    assert result.returncode == expected, (result.returncode, report.read_text(encoding="utf-8") if report.exists() else "no report")
    if not report.exists():
        return None
    text = report.read_text(encoding="utf-8")
    assert "[Missing text]" not in text and "缺少文案" not in text
    return json.loads(text)


def main():
    """Verify review binding, preservation, idempotence, rollback, restore and export portability."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--exe", type=pathlib.Path, default=BASE / "cpp/PhoneRecord.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-setup-测试-%!&'-") as temporary:
        root = pathlib.Path(temporary)
        with Phone() as phone:
            app = App(root, args.exe.resolve(), output, "native", phone_port=phone.port, password="synthetic-setup")
            before_record, before_actions = copy.deepcopy(phone.record), copy.deepcopy(phone.actions)
            plan = command(app, "--phone-setup-preview")
            assert plan["code"] == "SETUP_PLAN_READY" and len(plan["changes"]) == 7 and not phone.writes
            assert not any(path in ("/Start", "/Stop") for method, path in phone.requests)
            phone.record["KeepText"] += " changed"
            stale = command(app, "--phone-setup-apply", "--setup-plan", plan["plan_id"], expected=1)
            assert stale["code"] == "SETUP_PLAN_STALE" and not phone.writes
            phone.record = copy.deepcopy(before_record)
            checks.append("preview uses GET only; apply requires a matching fresh plan and never sends Start/Stop")

            result = command(app, "--phone-setup-apply", "--setup-plan", plan["plan_id"])
            assert result["code"] == "SETUP_VERIFIED" and result["applied"] and not result["real_call_verified"]
            assert phone.record["EnableRecord_RW"] == "1" and phone.record["CSE_RecordType_RW"] == "1" and phone.record["CSE_RecordVoiceCodec_R"] == "PCMA"
            assert phone.record["SYS_RecordAddr_RW"] == "127.0.0.1" and phone.record["SYS_RecordPort_RW"] == str(app.control)
            assert phone.record["OtherToggle_RW"] == before_record["OtherToggle_RW"] and phone.record["KeepText"] == before_record["KeepText"]
            assert phone.actions["AURL_CallEstablished_RW"] == f"http://127.0.0.1:{app.event}/event?ev=est"
            assert phone.actions["AURL_Registered_RW"] == before_actions["AURL_Registered_RW"] and phone.actions["KeepOther"] == before_actions["KeepOther"]
            backup = app.root / result["backup"]
            raw = backup.read_bytes()
            assert raw.startswith(b"dpapi:") and b"synthetic-only" not in raw and b"192.0.2.10" not in raw
            checks.append("apply configures seven recording fields, preserves unrelated Unicode/checkbox/company fields and encrypts the pre-write backup")

            count = len(phone.writes)
            matched = command(app, "--phone-setup-preview")
            assert matched["code"] == "SETUP_ALREADY_MATCHED" and not matched["changes"] and len(phone.writes) == count
            restored = command(app, "--phone-setup-restore", str(backup))
            assert restored["code"] == "SETUP_RESTORED" and phone.record == before_record and phone.actions == before_actions
            checks.append("matching settings are idempotent; current-user backup restores the exact prior forms")

            phone.corrupt_once = True
            failed = command(app, "--phone-setup-apply", "--setup-plan", plan["plan_id"], expected=1)
            assert failed["code"] == "SETUP_FAILED_RESTORED" and failed["rollback_verified"]
            assert phone.record == before_record and phone.actions == before_actions
            checks.append("unexpected readback changes cause bounded rollback and verified restoration")

            phone.deny = True
            denied = command(app, "--phone-setup-apply", "--setup-plan", plan["plan_id"], expected=1)
            assert denied["cause"] == "SETUP_WRITE_DENIED" and denied["rollback_verified"]
            assert phone.record == before_record and phone.actions == before_actions
            phone.deny = False
            checks.append("read-only web permissions report a specific cause without claiming settings were applied")

            count = len(phone.writes)
            phone.actions["AURL_CallEstablished_RW"] = "https://example.test/company-event"
            assert command(app, "--phone-setup-preview", expected=1)["code"] == "SETUP_CALLBACK_CONFLICT"
            phone.actions = copy.deepcopy(before_actions)
            phone.schema = False
            assert command(app, "--phone-setup-preview", expected=1)["code"] == "SETUP_UNSUPPORTED"
            phone.schema = True
            phone.status = "1"
            assert command(app, "--phone-setup-preview", expected=1)["code"] == "SETUP_PHONE_UNAVAILABLE_OR_BUSY"
            phone.status = "4"
            phone.record["DefaultRecord"] = "Start"
            assert command(app, "--phone-setup-preview", expected=1)["code"] == "SETUP_UNSUPPORTED"
            phone.record.pop("DefaultRecord")
            assert len(phone.writes) == count
            checks.append("unrelated target callbacks, unknown profiles and active recording states refuse writes")

            bad = app.root / "invalid.dpapi"
            bad.write_text("not a protected backup", encoding="ascii")
            assert command(app, "--phone-setup-restore", str(bad), expected=1)["code"] == "SETUP_BACKUP_INVALID"
            assert len(phone.writes) == count
            with Phone() as another:
                other = App(root, app.exe, output, "other_phone", phone_port=another.port, password="synthetic-setup")
                assert command(other, "--phone-setup-restore", str(backup), expected=1)["code"] == "SETUP_BACKUP_INVALID"
                assert not another.writes
            with app:
                denied = command(app, "--phone-setup-preview", expected=3)
                assert denied["code"] == "SETUP_CLOSE_APPLICATION"
            checks.append("invalid backups and a running product instance stop before any device write")

            for language in ("zh-CN", "zh-TW", "en"):
                pathlib.Path(str(app.config) + ".ui").write_text("language=" + language + "\n", encoding="utf-8")
                script = app.root / f"PhoneRecord-phone-setup-{language}.cmd"
                command(app, "--offline", "--export-phone-setup-script", str(script))
                lines = script.read_text(encoding="ascii").splitlines()
                encoded = next(line.split("-EncodedCommand ")[1] for line in lines if "-EncodedCommand " in line)
                assert max(map(len, lines)) < 8191
                decoded = base64.b64decode(encoded).decode("utf-16le")
                assert "APPLY" in decoded and "--phone-setup-preview" in decoded and "--setup-plan" in decoded
                assert "synthetic-setup" not in decoded and "127.0.0.1" not in decoded
                ps = app.root / "parse.ps1"
                ps.write_text("$s=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + encoded + "'));$tokens=$null;$errors=$null;[Management.Automation.Language.Parser]::ParseInput($s,[ref]$tokens,[ref]$errors)|Out-Null;if($errors.Count){exit 1}", encoding="ascii")
                parsed = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps)], startupinfo=startup_info(), creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
                assert parsed.returncode == 0
            checks.append("three-language ASCII CMD exports parse on Windows PowerShell with Unicode/special paths and embed no phone credentials or addresses")
    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False, "exe_sha256": hashlib.sha256(args.exe.resolve().read_bytes()).hexdigest(), "checks": checks}
    (output / "setup_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
