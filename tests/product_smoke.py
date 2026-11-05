"""Exercise the built C++ product with loopback-only synthetic protocol fixtures.

All generated recordings are synthetic. No real phone is contacted and no
fixture can establish that a real call was recorded. Temporary configurations
and synthetic credentials are removed after each run; only redacted evidence
and optional desktop captures are retained.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import http.client
import json
import pathlib
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

#: Source root containing the directly built native executable.
BASE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE / "src"))
import g711  # noqa: E402


def wait_for(predicate, timeout: float = 8):
    """Wait for a concrete local observation, raising instead of silently timing out."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.03)
    raise AssertionError("timed out waiting for local evidence")


def free_port() -> int:
    """Return an ephemeral TCP loopback port after releasing its reservation."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def udp_range() -> tuple[int, int]:
    """Find twelve free local UDP ports without contacting any remote address."""
    for _ in range(100):
        start = 24000 + secrets.randbelow(15000)
        probes = []
        try:
            for port in range(start, start + 12):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                probes.append(sock)
                sock.bind(("127.0.0.1", port))
            return start, start + 11
        except OSError:
            pass
        finally:
            for sock in probes:
                sock.close()
    raise AssertionError("no isolated UDP range available")


def startup_info():
    """Keep every native test window hidden and avoid altering the user's focus."""
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 0
    return info


def receive_line(sock: socket.socket, timeout: float = 2) -> bytes:
    """Read one bounded protocol response including its newline."""
    sock.settimeout(timeout)
    result = b""
    while len(result) < 8192:
        data = sock.recv(4096)
        if not data:
            break
        result += data
        if b"\n" in result:
            break
    return result


def start_frame(session: str, codec: str = "PCMA") -> bytes:
    """Build an explicitly synthetic Start using no caller names or phone numbers."""
    return (f"cmd=RecordStartRequest&id={session}&device=offline&remote_number=offline"
            f"&remote_name=&local_number=offline&local_name=offline&dir=in&codec={codec}").encode()


def parse_reply(reply: bytes) -> dict[str, str]:
    """Parse a small ASCII response from the actual C++ executable."""
    return dict(part.split("=", 1) for part in reply.decode().strip().split("&") if "=" in part)


def fingerprint(value: str) -> str:
    """Locate session artifacts using the product's non-identifying stable fingerprint."""
    number = 2166136261
    for byte in value.encode():
        number = ((number ^ byte) * 16777619) & 0xffffffff
    return f"s-{number >> 16:04x}-{number & 0xffff:04x}"


class App:
    """Own one isolated native process, its temporary configuration and safe evidence."""

    def __init__(self, root: pathlib.Path, exe: pathlib.Path, evidence: pathlib.Path,
                 name: str, *, offline: bool = True, gui: bool = False,
                 event_port: int | None = None, phone_port: int = 9, password: str = ""):
        """Prepare a dedicated loopback-only configuration without touching product settings."""
        self.root = root / name
        self.root.mkdir()
        self.exe = exe
        self.evidence = evidence
        self.name = name
        self.control = free_port()
        self.event = free_port() if event_port is None else event_port
        self.udp_start, self.udp_end = udp_range()
        self.config = self.root / "config.ini"
        self.config.write_text(
            f"phone_ip=127.0.0.1\nphone_port={phone_port}\nphone_user=offline\nphone_pass={password}\n"
            f"advertise_ip=127.0.0.1\ncontrol_port={self.control}\nhook_port={self.event}\n"
            f"udp_port_start={self.udp_start}\nudp_port_end={self.udp_end}\nidle_timeout=2\n"
            "record_dir=recordings\nledger_file=recordings\\calls.csv\ndirectory_file=offline.json\ntray_notifications=0\n",
            encoding="utf-8",
        )
        self.args = [str(exe), "--config", str(self.config), "--run-for", "60", "--export-report", str(self.root / "report.txt")]
        if offline:
            self.args.append("--offline")
        if not gui:
            self.args.append("--headless")
        self.process = None

    def launch(self):
        """Start the actual executable and wait for its explicit listener log."""
        self.process = subprocess.Popen(self.args, cwd=self.root, startupinfo=startup_info(), creationflags=subprocess.CREATE_NO_WINDOW)
        wait_for(lambda: "CONTROL_LISTENING" in self.log() or "CONTROL_BIND_FAILED" in self.log())
        return self

    def log(self) -> str:
        """Read the redacted technical log while allowing a concurrent append."""
        path = self.root / "PhoneRecord.log"
        return path.read_text(encoding="utf-8") if path.exists() else ""

    def connect(self) -> socket.socket:
        """Open a control connection to this isolated receiver only."""
        return socket.create_connection(("127.0.0.1", self.control), timeout=3)

    def request(self, frame: bytes) -> dict[str, str]:
        """Send one complete synthetic frame and return its real native response."""
        with self.connect() as sock:
            sock.sendall(frame + b"\r\n")
            return parse_reply(receive_line(sock))

    def stop_session(self, session: str):
        """Stop the exact synthetic session, preserving any other test sessions."""
        return self.request(f"cmd=RecordStopRequest&id={session}&reason=offline".encode())

    def metadata(self, session: str) -> tuple[pathlib.Path, dict]:
        """Wait for a finalized sidecar and return its corresponding WAV path."""
        suffix = "_" + fingerprint(session) + ".wav.quality.json"
        path = wait_for(lambda: next((self.root / "recordings").glob("*" + suffix), None) if (self.root / "recordings").exists() else None)
        content = wait_for(lambda: json.loads(path.read_text(encoding="utf-8")) if path.stat().st_size else None)
        return pathlib.Path(str(path).removesuffix(".quality.json")), content

    def shutdown(self):
        """Use the product's named stop signal and retain only redacted test evidence."""
        if self.process and self.process.poll() is None:
            signal = subprocess.run([str(self.exe), "--config", str(self.config), "--stop", "--headless"], startupinfo=startup_info(), creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
            assert signal.returncode == 0, "native stop signal failed"
            try:
                self.process.wait(timeout=12)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("native process failed to shut down cleanly")
        if self.process:
            assert self.process.returncode == 0, f"native exit code {self.process.returncode}"
        self.evidence.mkdir(parents=True, exist_ok=True)
        (self.evidence / f"{self.name}.log").write_text(self.log(), encoding="utf-8")
        report = self.root / "report.txt"
        if report.exists():
            (self.evidence / f"{self.name}.report.txt").write_text(report.read_text(encoding="utf-8"), encoding="utf-8")

    def __enter__(self):
        """Launch an owned native test process."""
        return self.launch()

    def __exit__(self, *_):
        """Always request graceful shutdown and release native resources."""
        self.shutdown()


def emit_rtp(port: int, codes: list[int], *, pt: int = 8, junk_first: bool = False):
    """Send explicitly synthetic 20ms payloads, optionally preceding them with junk UDP."""
    if junk_first:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as junk:
            junk.sendto(b"not RTP", ("127.0.0.1", port))
        time.sleep(0.03)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for sequence, code in enumerate(codes):
            payload = bytes([code]) * 160
            packet = struct.pack("!BBHII", 0x80, pt, sequence, sequence * 160, 0x53494d55) + payload
            sock.sendto(packet, ("127.0.0.1", port))
            time.sleep(0.003)


def http_get(port: int, path: str) -> tuple[int, str]:
    """Call a loopback HTTP fixture or event listener, closing each connection."""
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=4)) as connection:
        connection.request("GET", path)
        response = connection.getresponse()
        return response.status, response.read().decode()


class FakePhone:
    """A local challenge/CGI fixture, explicitly unrelated to the real phone."""

    def __init__(self, password: str, *, cached: bool = False):
        """Prepare synthetic credentials in memory and a private loopback HTTP server."""
        self.password = password
        self.cookie = secrets.token_hex(8)
        self.nonce = secrets.token_hex(8)
        self.cached = cached
        self.state = 4
        self.control_port = 0
        self.callback_port = 0
        self.compatibility = {"enabled": True, "mode": "1", "codec": "PCMA", "server": "127.0.0.1", "control": None, "schema": True, "actions": "normal"}
        self.requests = []
        self.start_calls = 0
        self.media_done = threading.Event()
        self.errors = []
        self.session = "synthetic-http-flow"
        owner = self

        class Handler(BaseHTTPRequestHandler):
            """Serve the existing challenge protocol without logging request bodies."""
            protocol_version = "HTTP/1.1"

            def answer(self, body: str, *, cookie: bool = False):
                """Send a complete close-delimited HTTP response."""
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                if cookie:
                    self.send_header("Set-Cookie", f"auth={owner.cookie}; path=/")
                self.end_headers()
                self.wfile.write(data)
                self.close_connection = True

            def do_GET(self):
                """Return synthetic session/nonce/state pages expected by the native client."""
                owner.requests.append((time.monotonic(), "GET", self.path.split("?")[0]))
                if self.path == "/":
                    self.answer("session", cookie=not owner.cached)
                elif self.path.startswith("/key==nonce"):
                    self.answer("" if owner.cached else owner.nonce)
                elif self.path == "/record.htm":
                    if self.headers.get("Cookie") != f"auth={owner.cookie}":
                        owner.errors.append("invalid protected-page cookie")
                    settings = owner.compatibility
                    checked = " checked" if settings["enabled"] else ""
                    fields = ""
                    if settings["schema"]:
                        fields = (f'<input type="checkbox" name="EnableRecord_RW" value="1"{checked}>'
                                  f'<select name="CSE_RecordType_RW"><option value="2">SIP</option><option selected value="{settings["mode"]}">Network</option></select>'
                                  f'<select name="CSE_RecordVoiceCodec_R"><option value="G722">G722</option><option selected value="{settings["codec"]}">Chosen</option></select>'
                                  f'<input name="SYS_RecordAddr_RW" value="\r\n{settings["server"]}\r\n">'
                                  f'<input name="SYS_RecordPort_RW" value="{owner.control_port if settings["control"] is None else settings["control"]}">')
                    self.answer(f'<script>var recordStatus = "{owner.state}";</script>' + fields)
                elif self.path == "/action.htm":
                    if owner.compatibility["actions"] == "login":
                        self.answer('<div id="logon_content">login</div>')
                    elif owner.compatibility["actions"] == "missing":
                        self.answer("no supported action fields")
                    else:
                        self.answer(''.join(f'<input name="{key}" value="\r\nhttp://127.0.0.1:{owner.callback_port}/event?ev={event}&amp;user=$active_user\r\n">' for key, event in (("AURL_CallEstablished_RW", "est"), ("AURL_CallTerminated_RW", "end"))))
                else:
                    owner.errors.append("unexpected GET endpoint")
                    self.answer("unknown")

            def do_POST(self):
                """Validate challenge input and drive only local synthetic recording traffic."""
                owner.requests.append((time.monotonic(), "POST", self.path))
                body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
                if self.path == "/":
                    from urllib.parse import parse_qs
                    fields = parse_qs(body)
                    expected = "offline:" + hashlib.md5(f"offline:{owner.password}:{owner.nonce}".encode()).hexdigest()
                    if fields.get("encoded") != [expected] or self.headers.get("Cookie") != f"auth={owner.nonce}":
                        owner.errors.append("challenge inputs differ from established protocol")
                    self.answer("authenticated")
                elif self.path == "/Start":
                    if body != "DefaultRecord=Start":
                        owner.errors.append("wrong Start body")
                    owner.start_calls += 1
                    owner.state = 1
                    self.answer("accepted")
                    threading.Thread(target=owner.media, daemon=True).start()
                elif self.path == "/Stop":
                    owner.state = 4
                    self.answer("stopped")
                    if owner.control_port:
                        with socket.create_connection(("127.0.0.1", owner.control_port), timeout=3) as sock:
                            sock.sendall(f"cmd=RecordStopRequest&id={owner.session}\r\n".encode())
                            receive_line(sock)
                else:
                    owner.errors.append("unexpected POST endpoint")
                    self.answer("unknown")

            def log_message(self, *_):
                """Suppress request/body output; evidence contains metadata only."""

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        """Return the fixture's private HTTP port."""
        return self.server.server_address[1]

    def media(self):
        """Exercise the formerly failing no-terminator/FIN path and synthetic RTP."""
        try:
            with socket.create_connection(("127.0.0.1", self.control_port), timeout=3) as control:
                control.sendall(start_frame(self.session))
                control.shutdown(socket.SHUT_WR)
                reply = parse_reply(receive_line(control))
            assert reply["result"] == "success"
            emit_rtp(int(reply["port"]), [0x80] * 50 + [0xA0] * 50 + [0x90] * 50)
        except Exception as exc:
            self.errors.append(type(exc).__name__)
        finally:
            self.media_done.set()

    def __enter__(self):
        """Start the owned fixture HTTP loop."""
        self.worker.start()
        return self

    def __exit__(self, *_):
        """Release fixture HTTP resources, including its listening socket."""
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(3)


def main() -> int:
    """Run high-value protocol, file, failure, authentication and lifecycle regressions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, default=BASE / "cpp" / "PhoneRecord.exe")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    exe, evidence = args.exe.resolve(strict=True), args.output.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    results = []

    def check(name, function):
        """Collect one independently meaningful result and keep running after failures."""
        started = time.monotonic()
        try:
            detail = function()
            item = {"case": name, "passed": True, "synthetic": True, "detail": detail}
        except Exception as exc:
            item = {"case": name, "passed": False, "synthetic": True, "error": type(exc).__name__ + ": " + str(exc)}
        item["seconds"] = round(time.monotonic() - started, 2)
        results.append(item)
        print(json.dumps(item, ensure_ascii=False), flush=True)

    with tempfile.TemporaryDirectory(prefix="PhoneRecord-product-") as temporary:
        root = pathlib.Path(temporary)

        def protocol():
            """Cover fragmented input, EOF, invalid codecs, oversized frames and duplicate Start."""
            with App(root, exe, evidence, "protocol") as app:
                frame = start_frame("fragmented")
                split = frame.index(b"&codec=")
                with app.connect() as sock:
                    sock.sendall(frame[:split])
                    time.sleep(.35)
                    sock.settimeout(.15)
                    try:
                        premature = sock.recv(4096)
                    except socket.timeout:
                        premature = b""
                    assert not premature, "fragment was dispatched before codec arrived"
                    sock.sendall(frame[split:] + b"\0")
                    first = parse_reply(receive_line(sock))
                    assert first["result"] == "success"
                    repeated = app.request(start_frame("fragmented"))
                    assert repeated["port"] == first["port"]
                app.stop_session("fragmented")
                rejected = app.request(start_frame("unsupported", "G722"))
                assert rejected["result"] == "failed" and rejected["reason"] == "unsupported_codec"
                with app.connect() as sock:
                    sock.sendall(b"x" * 9000)
                    sock.settimeout(3)
                    try:
                        assert not sock.recv(4096)
                    except ConnectionResetError:
                        pass
                assert app.request(start_frame("after-oversize"))["result"] == "success"
                app.stop_session("after-oversize")
                wait_for(lambda: "CTRL_FRAME_TOO_LARGE" in app.log())
            return "fragmented/NUL, duplicate port reuse, unsupported codec, frame bound and recovery"

        check("control_regressions", protocol)

        def audio():
            """Check native G.711 output, junk-source locking and signal/provenance separation."""
            cases = [("varying-a", "PCMA", 8, [0x80] * 50 + [0xA0] * 50 + [0x90] * 50, "audio"),
                     ("constant-a", "PCMA", 8, [0x80] * 150, "constant"),
                     ("constant-tail", "PCMA", 8, [0x80] * 150 + [0xD5] * 25, "constant"),
                     ("silence-a", "PCMA", 8, [0xD5] * 150, "silence"),
                     ("varying-u", "PCMU", 0, [0x80] * 50 + [0x90] * 50 + [0xA0] * 50, "audio")]
            with App(root, exe, evidence, "audio") as app:
                details = []
                for session, codec, pt, codes, expected in cases:
                    reply = app.request(start_frame(session, codec))
                    emit_rtp(int(reply["port"]), codes, pt=pt, junk_first=True)
                    app.stop_session(session)
                    path, metadata = app.metadata(session)
                    assert metadata["quality"] == expected, metadata
                    assert metadata["source"] == "test_or_other_peer", metadata
                    assert metadata["packets"] == len(codes) and metadata["invalid"] == 1, metadata
                    with wave.open(str(path), "rb") as wav:
                        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 8000, len(codes) * 160)
                        actual = wav.readframes(wav.getnframes())
                    encoded = b"".join(bytes([code]) * 160 for code in codes)
                    assert actual == g711.decode(encoded, codec), "native decoder differs from existing reference"
                    details.append({"fixture": session, "quality": expected, "provenance": metadata["source"]})
            return details

        check("synthetic_rtp_and_wav", audio)

        def ordering():
            """Verify loss, duplicate and out-of-order counters using actual UDP traffic."""
            with App(root, exe, evidence, "ordering") as app:
                reply = app.request(start_frame("ordering"))
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    for sequence in (0, 0, 2, 1, 3):
                        sock.sendto(struct.pack("!BBHII", 0x80, 8, sequence, sequence * 160, 1) + b"\x80" * 160, ("127.0.0.1", int(reply["port"])))
                        time.sleep(.02)
                app.stop_session("ordering")
                _, metadata = app.metadata("ordering")
                assert (metadata["packets"], metadata["lost"], metadata["duplicates"], metadata["out_of_order"]) == (3, 1, 1, 1), metadata
            return "3 accepted, 1 lost, 1 duplicate, 1 out of order"

        check("rtp_sequence_diagnostics", ordering)

        def media_clock():
            """Separate source clock gaps from loss, handling rollover and payload size changes."""
            with App(root, exe, evidence, "media_clock") as app:
                reply = app.request(start_frame("media-clock"))
                packets = [(65534, 0xffffff00, 160, 1), (65535, 0xffffffa0, 160, 1),
                           (0, 0x40, 160, 1), (1, 0x400, 160, 1), (2, 0x400, 160, 1),
                           (3, 0x4a0, 80, 1), (4, 0x4f0, 160, 1),
                           (5, 0x100000, 160, 2), (6, 0x1000a0, 160, 2)]
                encoded = b""
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    for sequence, timestamp, count, ssrc in packets:
                        payload = b"\x80" * count
                        encoded += payload
                        sock.sendto(struct.pack("!BBHII", 0x80, 8, sequence, timestamp, ssrc) + payload,
                                    ("127.0.0.1", int(reply["port"])))
                        time.sleep(.02)
                app.stop_session("media-clock")
                path, metadata = app.metadata("media-clock")
                assert metadata["lost"] == 0 and metadata["packets"] == len(packets), metadata
                assert metadata["timestamp_discontinuities"] == 2, metadata
                assert metadata["timestamp_gap_samples"] == 800, metadata
                assert metadata["timestamp_overlap_or_reset"] == 1 and metadata["ssrc_changes"] == 1, metadata
                with wave.open(str(path), "rb") as wav:
                    assert wav.readframes(wav.getnframes()) == g711.decode(encoded, "PCMA"), "diagnostics changed audio"
                # Final metadata is written before the session's summary log. Wait for
                # that independent completion evidence instead of racing the worker.
                wait_for(lambda: "RTP_CLOCK_DISCONTINUITY" in app.log() and "RTP_CLOCK_SUMMARY" in app.log())
            return "zero loss with clock gap/overlap detected; sequence/timestamp wrap and source switch handled; PCM unchanged"

        check("rtp_media_clock_diagnostics", media_clock)

        def storage_failure():
            """An unusable output path must preserve the distinction between RTP and WAV."""
            app = App(root, exe, evidence, "storage_failure")
            (app.root / "recordings").write_text("synthetic path blocker", encoding="utf-8")
            with app:
                reply = app.request(start_frame("storage-failure"))
                emit_rtp(int(reply["port"]), [0x80] * 5)
                wait_for(lambda: "reason=WAV_OPEN_FAILED" in app.log())
                assert "RTP_FIRST" in app.log()
            report = (evidence / "storage_failure.report.txt").read_text(encoding="utf-8")
            assert "rtp=1" in report and "files=0" in report and "Code: WAV_OPEN_FAILED" in report, report[:600]
            return "valid RTP observed; failed output path explicitly reported; no WAV success"

        check("storage_failure_diagnostics", storage_failure)

        def large_payload():
            """Decode a payload larger than the old 1,024-sample stack array safely."""
            with App(root, exe, evidence, "large_payload") as app:
                reply = app.request(start_frame("large-payload"))
                payload = bytes(range(256)) * 8
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.sendto(struct.pack("!BBHII", 0x80, 8, 1, 0, 1) + payload, ("127.0.0.1", int(reply["port"])))
                time.sleep(.05)
                app.stop_session("large-payload")
                path, metadata = app.metadata("large-payload")
                with wave.open(str(path), "rb") as wav:
                    assert wav.readframes(wav.getnframes()) == g711.decode(payload, "PCMA")
                assert metadata["packets"] == 1
            return "2,048-byte payload decoded byte-for-byte, no buffer overflow"

        check("large_rtp_payload_regression", large_payload)

        def unicode_delivery():
            """Verify real filesystem writes for UTF-8 paths and synthetic caller names."""
            app = App(root, exe, evidence, "unicode_delivery")
            app.config.write_text(app.config.read_text(encoding="utf-8").replace("recordings", "录音资料"), encoding="utf-8")
            with app:
                frame = start_frame("unicode").replace(b"&remote_name=&", "&remote_name=测试来源&".encode())
                reply = app.request(frame)
                emit_rtp(int(reply["port"]), [0x80] * 50 + [0xA0] * 50 + [0x90] * 50)
                app.stop_session("unicode")
                folder = app.root / "录音资料"
                metadata = wait_for(lambda: next(folder.glob("*.quality.json"), None) if folder.exists() else None)
                assert "测试来源" in metadata.name
                content = json.loads(metadata.read_text(encoding="utf-8"))
                assert content["quality"] == "audio" and content["source"] == "test_or_other_peer"
                assert "测试来源" in (folder / "calls.csv").read_text(encoding="utf-8-sig")
            return "UTF-8 folder, WAV filename, metadata and quoted ledger preserve synthetic Chinese names"

        check("unicode_delivery", unicode_delivery)

        def bounded_log():
            """Verify real on-disk rotation at the configured maximum size."""
            app = App(root, exe, evidence, "log_rotation")
            (app.root / "PhoneRecord.log").write_bytes(b"x" * (2 * 1024 * 1024))
            with app:
                assert (app.root / "PhoneRecord.log.1").stat().st_size == 2 * 1024 * 1024
                assert (app.root / "PhoneRecord.log").stat().st_size < 100000
            return "2 MiB legacy log rotated; current log stays bounded"

        check("bounded_diagnostic_log", bounded_log)

        def no_media():
            """A successful control response without RTP must produce no success WAV."""
            with App(root, exe, evidence, "no_media") as app:
                assert app.request(start_frame("no-media"))["result"] == "success"
                wait_for(lambda: "SESSION_NO_AUDIO" in app.log(), 12)
                assert not list((app.root / "recordings").glob("*.wav"))
                assert "RTP_NOT_RECEIVED" in app.log()
            return "control success followed by explicit RTP_NOT_RECEIVED, no WAV"

        check("control_is_not_audio_success", no_media)

        def conflict():
            """An occupied Action URL port must stay visible as EVENT_BIND_FAILED."""
            with socket.socket() as reserved:
                reserved.bind(("127.0.0.1", 0))
                reserved.listen()
                with App(root, exe, evidence, "event_conflict", event_port=reserved.getsockname()[1]) as app:
                    wait_for(lambda: "EVENT_BIND_FAILED" in app.log())
                assert "Code: EVENT_BIND_FAILED" in (evidence / "event_conflict.report.txt").read_text(encoding="utf-8")
            return "occupied port retained; native report identifies event bind failure"

        check("event_port_conflict", conflict)

        def challenge():
            """Run the complete native HTTP-to-control-to-RTP flow against the local fixture."""
            password = secrets.token_urlsafe(16)
            with FakePhone(password) as phone:
                app = App(root, exe, evidence, "http_flow", offline=False, phone_port=phone.port, password=password)
                phone.control_port = app.control
                phone.callback_port = app.event
                with app:
                    wait_for(lambda: "AUTH_VERIFIED" in app.log(), 10)
                    wait_for(lambda: json.loads(http_get(app.event, "/compatibility")[1])["verdict"] == "ready", 8)
                    before_start = len(phone.requests)
                    assert http_get(app.event, "/event?ev=est")[0] == 200
                    assert http_get(app.event, "/event?ev=est")[0] == 200
                    wait_for(lambda: phone.media_done.is_set(), 10)
                    assert not any(method == "GET" for _, method, _ in phone.requests[before_start:]), "recent readiness was queried again at answer"
                    assert "recent_ready=1" in app.log(), "recent idle state was not reused"
                    assert http_get(app.event, "/event?ev=end")[0] == 200
                    _, metadata = app.metadata(phone.session)
                    assert metadata["quality"] == "audio" and metadata["source"] == "test_or_other_peer", metadata
                    assert phone.start_calls == 1, "duplicate event toggled Start twice"
                    assert not phone.errors, phone.errors
                    log = app.log()
                    assert log.index("EVENT_HTTP_ACK") < log.index("OPERATION_BEGIN operation=start")
                    assert "framing=eof" in log and "START_MEDIA_CONFIRMED" in log
                    for sensitive in (password, phone.nonce, phone.cookie):
                        assert sensitive not in log, "credential leaked to diagnostic log"
                intervals = [right[0] - left[0] for left, right in zip(phone.requests, phone.requests[1:])]
                assert min(intervals) >= 1.08, intervals
            return {"start_requests": 1, "minimum_http_interval": round(min(intervals), 3), "fin_path": True, "credential_redaction": True, "synthetic_only": True}

        check("full_native_trigger_flow", challenge)

        def startup_inspection_retry():
            """A call interrupts startup inspection, which resumes after idle even with health disabled."""
            password = secrets.token_urlsafe(16)
            with FakePhone(password) as phone:
                app = App(root, exe, evidence, "startup_retry", offline=False, phone_port=phone.port, password=password)
                app.config.write_text(app.config.read_text(encoding="utf-8") + "health_interval=0\n", encoding="utf-8")
                phone.control_port, phone.callback_port = app.control, app.event
                with app:
                    wait_for(lambda: "AUTH_VERIFIED" in app.log(), 10)
                    assert http_get(app.event, "/event?ev=est")[0] == 200
                    wait_for(lambda: json.loads(http_get(app.event, "/compatibility")[1])["verdict"] == "deferred", 5)
                    wait_for(lambda: phone.media_done.is_set(), 10)
                    assert http_get(app.event, "/event?ev=end")[0] == 200
                    wait_for(lambda: "STOP_COMPLETE" in app.log(), 8)
                    wait_for(lambda: json.loads(http_get(app.event, "/health")[1])["active_sessions"] == 0)
                    before_retry = len(phone.requests)
                    wait_for(lambda: json.loads(http_get(app.event, "/compatibility")[1])["verdict"] == "ready", 28)
                    assert phone.start_calls == 1 and not phone.errors, phone.errors
                    assert all(method == "GET" for _, method, _ in phone.requests[before_retry:]), "idle retry wrote to phone"
            return "startup inspection yields to call, then completes automatically after 20-second idle with health_interval=0"

        check("startup_inspection_idle_retry", startup_inspection_retry)

        def stale_start_guard():
            """An observed busy state must retain the original query/reset/Start safety path."""
            password = secrets.token_urlsafe(16)
            with FakePhone(password) as phone:
                phone.state = 1
                app = App(root, exe, evidence, "stale_start", offline=False, phone_port=phone.port, password=password)
                phone.control_port = app.control
                phone.callback_port = app.event
                with app:
                    wait_for(lambda: "AUTH_VERIFIED" in app.log(), 10)
                    wait_for(lambda: json.loads(http_get(app.event, "/compatibility")[1])["verdict"] == "ready", 8)
                    assert http_get(app.event, "/event?ev=est")[0] == 200
                    wait_for(lambda: phone.media_done.is_set(), 15)
                    assert phone.start_calls == 1 and "START_STALE_STATE" in app.log(), app.log()[-1500:]
                    assert "recent_ready=0" in app.log()
                    posts = [path for _, method, path in phone.requests if method == "POST"]
                    assert posts.index("/Stop") < posts.index("/Start"), posts
                    assert http_get(app.event, "/event?ev=end")[0] == 200
                    wait_for(lambda: "STOP_COMPLETE" in app.log())
                    wait_for(lambda: json.loads(http_get(app.event, "/health")[1])["active_sessions"] == 0)
                    # Stop invalidates the earlier ready page. A following call needs
                    # a fresh protected-page query even while its age would be valid.
                    previous_queries = len([p for _, m, p in phone.requests if m == "GET" and p == "/record.htm"])
                    phone.media_done.clear()
                    assert http_get(app.event, "/event?ev=est")[0] == 200
                    wait_for(lambda: phone.start_calls == 2, 8)
                    wait_for(lambda: phone.media_done.is_set(), 8)
                    assert len([p for _, m, p in phone.requests if m == "GET" and p == "/record.htm"]) == previous_queries + 1
                    assert http_get(app.event, "/event?ev=end")[0] == 200
                    wait_for(lambda: app.log().count("STOP_COMPLETE") == 2)
                    assert not phone.errors, phone.errors
            return "busy state reset retained; Stop forces fresh query on the next call"

        check("start_guard_invalidation", stale_start_guard)

        def cache():
            """Reproduce the previously inverted successful-cache-validation branch."""
            password = secrets.token_urlsafe(16)
            with FakePhone(password, cached=True) as phone:
                app = App(root, exe, evidence, "auth_cache", offline=False, phone_port=phone.port, password=password)
                (app.root / ".phone_cookie").write_text(phone.cookie, encoding="utf-8")
                with app:
                    wait_for(lambda: "AUTH_CACHE_VALID" in app.log(), 8)
                    assert not phone.errors, phone.errors
                    assert (app.root / ".phone_cookie").read_text(encoding="utf-8").startswith("dpapi:")
                assert all(method != "POST" for _, method, _ in phone.requests), "cache branch unexpectedly reauthenticated"
            return "empty nonce + valid cached session accepted and stored using DPAPI"

        check("cached_authentication_regression", cache)

    manifest = {"synthetic": True, "real_phone_contacted": False, "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "results": results}
    (evidence / "results.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"PASSED {sum(item['passed'] for item in results)}/{len(results)}", flush=True)
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
