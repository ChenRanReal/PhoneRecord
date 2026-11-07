"""Verify three-language native UI, embedded help and positive-integer fields offline.

Only owned child windows receive messages. Replacement tests exercise the same
validation as paste without reading or changing the user's clipboard.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import pathlib
import re
import tempfile
import time

from PIL import Image

from desktop_smoke import USER, dismiss_dialog, set_text, title
from desktop_workflow import main_window
from product_smoke import App, BASE, emit_rtp, start_frame, wait_for


def replace_selection(window, value: str):
    """Send an actual native selection replacement with Windows cross-process marshalling."""
    USER.SendMessageW(window, 0x00B1, 0, -1)
    value_buffer = ctypes.create_unicode_buffer(value)
    USER.SendMessageW(window, 0x00C2, True, ctypes.addressof(value_buffer))


def select_language(window, index: int):
    """Select one language using the product's actual dropdown notification path."""
    USER.SendMessageW(USER.GetDlgItem(window, 430), 0x014E, index, 0)
    USER.SendMessageW(window, 0x0111, (1 << 16) | 430, USER.GetDlgItem(window, 430))


def select_help(window, topic: int) -> str:
    """Read the embedded topic after native page and selection notifications."""
    USER.SendMessageW(window, 0x0111, 104, 0)
    USER.SendMessageW(USER.GetDlgItem(window, 800), 0x014E, topic, 0)
    USER.SendMessageW(window, 0x0111, (1 << 16) | 800, USER.GetDlgItem(window, 800))
    return title(USER.GetDlgItem(window, 801))


def capture_page(window, output: pathlib.Path, name: str, page: int):
    """Convert the same-process offline native render, including the new Help page."""
    USER.SendMessageW(window, 0x0111, 100 + page, 0)
    USER.SendMessageW(window, 0x0111, 9900, 0)
    path = output / f"PhoneRecord-{page}.bmp"
    with Image.open(path) as source:
        source.convert("RGB").save(output / f"{name}.png")


def resources() -> int:
    """Check every compiled resource has exactly three nonempty locale strings and a unique key."""
    quoted = r'"(?:\\.|[^"\\])*"'
    expression = re.compile(r'^\s*\{(' + quoted + r'), L(' + quoted + r'), L(' + quoted + r'), L(' + quoted + r')\},$', re.M)
    content = (BASE / "cpp/ui_resources.hpp").read_text(encoding="utf-8")
    content += "\n" + (BASE / "cpp/ai_resources.inc").read_text(encoding="utf-8")
    entries = expression.findall(content)
    assert len(entries) >= 216
    keys = set()
    for row in entries:
        key, *translations = [json.loads(value) for value in row]
        assert key not in keys and all(value.strip() for value in translations), key
        assert all("\ufffd" not in value and "[Missing text]" not in value for value in translations), key
        keys.add(key)
    for file in (BASE / "cpp").glob("*.hpp"):
        for key in re.findall(r'(?:uiText|button|localizedControl)\((?:L"\w+",\s*)?"([^"\n]+)"', file.read_text(encoding="utf-8")):
            assert key in keys, (file.name, key)
    return len(entries)


def main() -> int:
    """Verify real native labels, input behavior, field help, language persistence and live sessions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--exe", type=pathlib.Path, default=BASE / "cpp/PhoneRecord.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = args.exe.resolve(strict=True)
    count = resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-localization-") as temporary:
        root = pathlib.Path(temporary)
        app = App(root, exe, output, "localization", gui=True)
        app.args += ["--capture-dir", str(output)]
        app.config.write_text(app.config.read_text(encoding="utf-8").replace("idle_timeout=2", "idle_timeout=20"), encoding="utf-8")
        before = app.config.read_bytes()
        with app:
            window = main_window(app)
            port = USER.GetDlgItem(window, 406)
            for control in (401, 405, 406, 407, 408):
                field = USER.GetDlgItem(window, control)
                previous = title(field)
                for invalid in ("-1", "1.5", "80a", "65536", "0", "１２", " 80 "):
                    replace_selection(field, invalid)
                    assert title(field) == previous, (control, invalid)
                replace_selection(field, "18080")
                assert title(field) == "18080"
                set_text(field, previous)
            set_text(port, "")
            USER.SendMessageW(port, 0x0102, ord("0"), 0)
            assert title(port) == ""
            USER.SendMessageW(port, 0x0102, ord("8"), 0)
            USER.SendMessageW(port, 0x0102, ord("0"), 0)
            assert title(port) == "80"
            set_text(port, str(app.event))
            checks.append("all five port inputs reject signed/decimal/non-digit/out-of-range replacements; typing enforces positive values")

            session = "language-switch-active"
            reply = app.request(start_frame(session))
            emit_rtp(int(reply["port"]), [0x80] * 50 + [0xa0] * 50 + [0x90] * 50)
            set_text(USER.GetDlgItem(window, 400), "192.0.2.20")
            set_text(USER.GetDlgItem(window, 403), "synthetic-unsaved-password", verify=False)
            expected = [(0, "连接设置", "话机 IPv4 地址", "帮助与适配"),
                        (1, "連接設定", "話機 IPv4 位址", "說明與適配"),
                        (2, "Settings", "Phone IPv4 address", "Help & devices")]
            for language, settings, phone_label, help_label in expected:
                select_language(window, language)
                assert title(USER.GetDlgItem(window, 103)) == settings
                assert title(USER.GetDlgItem(window, 700)) == phone_label
                assert title(USER.GetDlgItem(window, 104)) == help_label
                assert title(USER.GetDlgItem(window, 400)) == "192.0.2.20"
                assert app.config.read_bytes() == before, "language change saved connection drafts"
                assert app.request(start_frame(session))["port"] == reply["port"], "language switch recreated the recording session"
                body = select_help(window, 1)
                for key in ("phone_ip", "phone_port", "phone_pass_dpapi", "advertise_ip", "control_port", "hook_port", "udp_port_start", "udp_port_end", "idle_timeout", "record_dir", "close_action", "health_interval"):
                    assert f"[{key}]" in body, (language, key)
                assert "[缺少文案]" not in body and "[Missing text]" not in body and "\\n" not in body
                select_help(window, 3)
                capture_page(window, output, f"language_{language}_devices", 4)
                capture_page(window, output, f"language_{language}_settings", 3)
            checks.append("all three languages update labels and full field help immediately without saving drafts or recreating an active recording")

            body = select_help(window, 3)
            assert "Fanvil X3S" in body and "2.4.7.9.1" in body and "Candidates requiring validation" in body
            assert all(value in body for value in ("RecordStartRequest", "RecordStopRequest", "PCMA", "PCMU", "SIPREC", "G.722", "X210"))
            checks.append("embedded compatibility guide distinguishes verified device from candidates and lists required control, RTP, codec and trigger capabilities")
            set_text(port, "18080")
            body = select_help(window, 2)
            assert "http://<PC_IP>:<EVENT_PORT>/event?ev=est" in body and "http://<PC_IP>:<EVENT_PORT>/event?ev=end" in body
            for topic in range(7):
                help_body = select_help(window, topic)
                assert all(address not in help_body for address in ("127.0.0.1", "192.0.2.20")), "help leaked configured/draft addresses"
            assert str(app.event) != "18080" and app.config.read_bytes() == before
            checks.append("all help topics omit configured/draft IPs; callback templates contain placeholders only")
            app.stop_session(session)
            app.metadata(session)
            set_text(USER.GetDlgItem(window, 400), "127.0.0.1")
            set_text(USER.GetDlgItem(window, 403), "", verify=False)
            set_text(port, "0")
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid, "hook_port")
            assert app.config.read_bytes() == before
            set_text(port, str(app.event))
            select_language(window, 1)
            USER.SendMessageW(window, 0x0111, (0x100 << 16) | 406, port)
            assert "18080" in title(USER.GetDlgItem(window, 722))
            checks.append("zero event port requires explicit external mode; inline field guidance follows the active language")
            assert pathlib.Path(str(app.config) + ".ui").read_text(encoding="utf-8") == "language=zh-TW\n"

        loaded = App(root, exe, output, "language_reload", gui=True)
        pathlib.Path(str(loaded.config) + ".ui").write_text("language=zh-TW\n", encoding="utf-8")
        with loaded:
            window = main_window(loaded)
            assert title(USER.GetDlgItem(window, 103)) == "連接設定"
        checks.append("separate language preference loads in a fresh native process")

        external = App(root, exe, output, "external_mode", gui=True)
        external.config.write_text(external.config.read_text(encoding="utf-8").replace(f"hook_port={external.event}", "hook_port=0"), encoding="utf-8")
        with external:
            window = main_window(external)
            assert title(USER.GetDlgItem(window, 406)) == "18080"
            assert USER.SendMessageW(USER.GetDlgItem(window, 413), 0x00F0, 0, 0) == 1
            assert "hook_port=0" in select_help(window, 1)
            assert "HTTP" in select_help(window, 2) and "/event?ev=" not in title(USER.GetDlgItem(window, 801))
        checks.append("legacy hook_port=0 is represented by an external-trigger checkbox with a positive disabled port field")
    result = {"passed": True, "synthetic_only": True, "real_phone_contacted": False,
              "resources_per_language": count, "languages": ["zh-CN", "zh-TW", "en"],
              "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "checks": checks}
    (output / "localization_results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
