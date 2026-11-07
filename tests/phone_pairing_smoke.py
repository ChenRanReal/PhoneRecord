"""Verify one-click DHCP pairing and real recording after a loopback phone changes address."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import socket
import struct
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

from phone_setup_smoke import Phone, command
from product_smoke import App, http_get, parse_reply, receive_line, start_frame, wait_for


def callback(port: int, token: str, event: str, source="127.0.0.2", extra="") -> int:
    """Send from a distinct loopback source; credentials never enter test output or evidence."""
    with socket.create_connection(("127.0.0.1", port), timeout=4, source_address=(source, 0)) as connection:
        path = f"/event?ev={event}&pair={token}{extra}"
        connection.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        response = b""
        while True:
            part = connection.recv(4096)
            if not part:
                break
            response += part
    return int(response.split(b" ", 2)[1])


def main() -> int:
    """Pair through actual setup commands, then test credential checks, rebind, RTP and restart recovery."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    checks = []
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-pairing-") as folder, Phone() as original:
        app = App(pathlib.Path(folder), args.exe.resolve(), output, "paired", phone_port=original.port, password="synthetic-setup", offline=False)
        with app.config.open("a", encoding="utf-8") as config:
            config.write("phone_address_mode=dhcp\nphone_mac=02-AA-BB-CC-DD-01\nhealth_interval=0\n")
        plan = command(app, "--phone-setup-preview")
        assert plan["code"] == "SETUP_PLAN_READY" and not original.writes
        result = command(app, "--phone-setup-apply", "--setup-plan", plan["plan_id"])
        assert result["code"] == "SETUP_VERIFIED" and result["applied"]
        tokens = []
        for key, event in (("AURL_CallEstablished_RW", "est"), ("AURL_CallTerminated_RW", "end"),
                           ("AURL_SetupCompleted_RW", "ready"), ("AURL_IpChange_RW", "ip")):
            query = parse_qs(urlsplit(original.actions[key]).query)
            assert query["ev"] == [event] and len(query["pair"][0]) == 64
            tokens.append(query["pair"][0])
        assert len(set(tokens)) == 1
        token = tokens[0]
        protected = app.config.with_suffix(".ini.pairing").read_text(encoding="ascii")
        assert protected.startswith("dpapi:") and token not in protected and token not in json.dumps(result)
        assert token not in app.log()
        checks.append("setup previews without writes; apply configures four authenticated callbacks and protects pairing")

        with Phone(host="127.0.0.2", port=original.port) as moved:
            moved.record = copy.deepcopy(original.record); moved.actions = copy.deepcopy(original.actions)
            finished = threading.Event(); errors = []

            def media():
                """Send actual control and RTP from the new address, without a real call."""
                try:
                    with socket.create_connection(("127.0.0.1", app.control), timeout=3, source_address=("127.0.0.2", 0)) as control:
                        control.sendall(start_frame("paired-new-address") + b"\r\n")
                        reply = parse_reply(receive_line(control))
                        assert reply["result"] == "success"
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                        udp.bind(("127.0.0.2", 0))
                        for sequence in range(30):
                            packet = b"\x80\x08" + struct.pack("!HII", sequence, sequence * 160, 1234) + bytes([0x80 if sequence % 2 else 0xA0]) * 160
                            udp.sendto(packet, ("127.0.0.1", int(reply["port"])))
                            time.sleep(0.02)
                except Exception as error:
                    errors.append(type(error).__name__)
                finally:
                    finished.set()

            def stop():
                """Finalize the exact synthetic session through the real control listener."""
                with socket.create_connection(("127.0.0.1", app.control), timeout=3, source_address=("127.0.0.2", 0)) as control:
                    control.sendall(b"cmd=RecordStopRequest&id=paired-new-address&reason=test\r\n")
                    receive_line(control)

            moved.on_start = media; moved.on_stop = stop
            with app:
                wait_for(lambda: "COMPATIBILITY_RESULT" in app.log(), 15)
                assert callback(app.event, "", "ip") == 403
                assert callback(app.event, "b" * 64, "est") == 403
                assert callback(app.event, "", "ip", source="127.0.0.1") == 200
                before = len(moved.requests)
                previous_auth = app.log().count("AUTH_VERIFIED")
                assert callback(app.event, token, "ip", extra="&ip=203.0.113.99") == 200
                wait_for(lambda: "PHONE_ADDRESS_CHANGED" in app.log(), 15)
                wait_for(lambda: len(moved.requests) > before, 10)
                wait_for(lambda: app.log().count("AUTH_VERIFIED") > previous_auth, 15)
                assert callback(app.event, token, "est") == 200
                wait_for(finished.is_set, 15)
                assert not errors, errors
                wait_for(lambda: "START_MEDIA_CONFIRMED" in app.log(), 10)
                assert callback(app.event, token, "end") == 200
                wait_for(lambda: "STOP_COMPLETE" in app.log(), 10)
                wait_for(lambda: json.loads(http_get(app.event, "/health")[1])["active_sessions"] == 0)
                wait_for(lambda: next((app.root / "recordings").glob("*.wav"), None))
                assert token not in app.log()
                assert "PHONE_IDENTITY_MISMATCH" not in app.log()
            checks.append("forged callbacks rejected; authenticated source overrides IP query; recording works from new target")
            moved_before = len(moved.requests); old_before = len(original.requests)
            with app:
                wait_for(lambda: len(moved.requests) > moved_before, 12)
                assert len(original.requests) == old_before
                assert callback(app.event, token, "ready") == 200
            checks.append("restart restores last target and pairing; no old-address login, DNS or repeated MAC verification")
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-pairing-keep-") as folder, Phone() as reserved:
        reserved.actions["AURL_SetupCompleted_RW"] = "https://example.test/setup"
        reserved.actions["AURL_IpChange_RW"] = "https://example.test/ip-change"
        kept = App(pathlib.Path(folder), args.exe.resolve(), output, "kept", phone_port=reserved.port, password="synthetic-setup", offline=False)
        with kept.config.open("a", encoding="utf-8") as config:
            config.write("phone_address_mode=dhcp\nphone_mac=02-AA-BB-CC-DD-01\nhealth_interval=0\n")
        plan = command(kept, "--phone-setup-preview")
        assert plan["code"] == "SETUP_PLAN_READY"
        assert "AURL_SetupCompleted_RW" not in plan["changes"] and "AURL_IpChange_RW" not in plan["changes"]
        result = command(kept, "--phone-setup-apply", "--setup-plan", plan["plan_id"])
        assert result["code"] == "SETUP_VERIFIED" and result["applied"]
        assert reserved.actions["AURL_SetupCompleted_RW"] == "https://example.test/setup"
        assert reserved.actions["AURL_IpChange_RW"] == "https://example.test/ip-change"
        for key, event in (("AURL_CallEstablished_RW", "est"), ("AURL_CallTerminated_RW", "end")):
            query = parse_qs(urlsplit(reserved.actions[key]).query)
            assert query["ev"] == [event] and len(query["pair"][0]) == 64
        checks.append("existing startup and IP-change URLs stay unchanged while call callbacks gain pairing")
    (output / "pairing-results.json").write_text(json.dumps({"passed": checks, "real_phone_settings_changed": False}, indent=2) + "\n", encoding="utf-8")
    print(f"PHONE_PAIRING_SMOKE_OK checks={len(checks)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
