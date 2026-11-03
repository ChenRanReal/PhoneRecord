"""Move only the recording callbacks to a new event port, preserving other phone fields.

Plan mode is read-only. Apply requires the recorder already listening on the
new port and idle. The current C++ session cache is reused; this tool never
logs in, changes credentials or displays callback query values.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import pathlib
import re
import socket
import tempfile
import time
from ctypes import wintypes as w
from html.parser import HTMLParser
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import urlopen

#: The only device fields this operation is allowed to alter.
TARGETS = {"AURL_CallEstablished_RW": "est", "AURL_CallTerminated_RW": "end"}
#: Form submission controls are not persisted device configuration.
SUBMISSION_KEYS = {"ReturnPage", "DefaultSubmit"}


class Blob(ctypes.Structure):
    """Native DPAPI byte buffer, used only to protect the local rollback snapshot."""
    _fields_ = [("size", w.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]


def protect(data: bytes, *, decrypt: bool = False) -> bytes:
    """Protect/unprotect current-user bytes without emitting sensitive content."""
    array = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    source, output = Blob(len(data), array), Blob()
    crypt = ctypes.WinDLL("crypt32", use_last_error=True)
    function = crypt.CryptUnprotectData if decrypt else crypt.CryptProtectData
    function.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, w.DWORD, ctypes.POINTER(Blob)]
    if not function(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output)):
        raise RuntimeError("current-user credential protection failed")
    kernel = ctypes.WinDLL("kernel32")
    kernel.LocalFree.argtypes = [w.HLOCAL]
    try:
        return ctypes.string_at(output.data, output.size)
    finally:
        kernel.LocalFree(output.data)


class Form(HTMLParser):
    """Capture the phone's existing complete input-field contract with HTML entities decoded."""
    def __init__(self):
        """Initialize one transient form snapshot without durable plaintext storage."""
        super().__init__(convert_charrefs=True)
        self.fields = {}

    def handle_starttag(self, tag, attributes):
        """Retain named fields while excluding button/submit transport controls."""
        values = dict(attributes)
        if tag.lower() == "input" and values.get("name") and values.get("type", "text").lower() not in ("button", "submit", "reset"):
            # This firmware wraps input values in CR/LF for HTML layout. Match the
            # established device form contract by comparing/submitting logical values.
            self.fields[values["name"]] = (values.get("value") or "").strip()


def callback_url(value: str, address: str, old: int, new: int, event: str) -> str:
    """Change only a validated local authority port, retaining every path/query character."""
    parsed = urlsplit(value)
    if parsed.scheme != "http" or parsed.hostname != address or parsed.port != old or parsed.username or parsed.path != "/event" or parse_qs(parsed.query).get("ev") != [event]:
        raise RuntimeError("recording callback differs from the expected local event endpoint")
    prefix = f"http://{address}:{old}"
    if not value.startswith(prefix + "/event"):
        raise RuntimeError("callback authority cannot be replaced exactly")
    return f"http://{address}:{new}" + value[len(prefix):]


class Client:
    """One paced raw HTTP connection at a time, reusing only the C++ validated cookie."""
    def __init__(self, config: pathlib.Path):
        """Read target metadata and reuse an existing session; a password is never used."""
        values = dict(line.split("=", 1) for line in config.read_text(encoding="utf-8").splitlines() if "=" in line and not line.startswith(("#", ";")))
        self.address = values["advertise_ip"].strip()
        self.host = values["phone_ip"].strip()
        self.phone_port = int(values.get("phone_port", "80"))
        self.event_port = int(values["hook_port"])
        cache = (config.parent / ".phone_cookie").read_text(encoding="utf-8").strip()
        if cache.startswith("dpapi:"):
            import base64
            self.cookie = protect(base64.b64decode(cache[6:]), decrypt=True).decode("utf-8")
        else:
            self.cookie = cache
        if not self.cookie or len(self.cookie) > 128 or "\r" in self.cookie or "\n" in self.cookie:
            raise RuntimeError("validated C++ session cache unavailable")
        self.completed = time.monotonic()

    def exchange(self, fields: dict | None = None) -> dict:
        """Read or submit /action.htm once, with a quiet interval and no raw HTTP output."""
        time.sleep(max(0, self.completed + 1.2 - time.monotonic()))
        method = "GET" if fields is None else "POST"
        body = b"" if fields is None else urlencode(fields).encode("ascii")
        head = f"{method} /action.htm HTTP/1.1\r\nHost: {self.host}\r\nUser-Agent: Mozilla/5.0\r\nReferer: http://{self.host}/action.htm\r\nCookie: auth={self.cookie}\r\nConnection: close\r\n"
        if fields is not None:
            head += f"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: {len(body)}\r\n"
        try:
            with socket.create_connection((self.host, self.phone_port), timeout=3) as stream:
                stream.sendall(head.encode("ascii") + b"\r\n" + body)
                response = bytearray()
                deadline = time.monotonic() + 6
                while True:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("complete phone response timed out")
                    stream.settimeout(max(.01, deadline - time.monotonic()))
                    block = stream.recv(65536)
                    if not block:
                        break
                    response += block
                    if len(response) > 2 * 1024 * 1024:
                        raise RuntimeError("phone response exceeded limit")
        finally:
            self.completed = time.monotonic()
        headers, raw = bytes(response).split(b"\r\n\r\n", 1)
        if not headers.split(b"\r\n", 1)[0].startswith(b"HTTP/1.1 200"):
            raise RuntimeError("phone did not acknowledge the action page; no retry performed")
        if b"chunked" in headers.lower():
            raw = re.sub(rb"\r\n[0-9a-fA-F]{1,8}\r\n", b"\r\n", raw)
            raw = re.sub(rb"^[0-9a-fA-F]{1,8}\r?\n", b"", raw)
        if b"logon_content" in raw or b"Not Authorized" in raw:
            raise RuntimeError("phone session expired; no retry performed")
        if fields is not None:
            return {}
        form = Form()
        form.feed(raw.decode("utf-8", "strict"))
        if not all(key in form.fields for key in TARGETS):
            raise RuntimeError("action page or session invalid; run the product's phone check first")
        return form.fields


def migrate(client: Client, old: int, new: int, apply: bool) -> dict:
    """Plan two exact edits; apply only after local readiness and verify every preserved field."""
    before = client.exchange()
    after = dict(before)
    for field, event in TARGETS.items():
        after[field] = callback_url(before[field], client.address, old, new, event)
    plan = {"old_port": old, "new_port": new, "changed_fields": list(TARGETS),
            "preserved_fields": len(set(before) - set(TARGETS) - SUBMISSION_KEYS), "apply": apply}
    if not apply:
        return plan
    if client.event_port != new:
        raise RuntimeError("save and restart the application on the new port before applying callbacks")
    with urlopen(f"http://127.0.0.1:{new}/health", timeout=3) as response:
        health = json.load(response)
    if not health.get("event") or not health.get("control") or health.get("call_active") or health.get("active_sessions") or health.get("phone_health") != 1:
        raise RuntimeError("new recorder is not verified and idle; no device write performed")
    rollback = pathlib.Path(tempfile.mkdtemp(prefix="PhoneRecord-event-port-")) / "action-fields.dpapi"
    rollback.write_bytes(protect(json.dumps(before, ensure_ascii=False).encode("utf-8")))
    submission = {**after, "ReturnPage": "/action.htm", "DefaultSubmit": "Apply"}
    try:
        client.exchange(submission)
        verified = client.exchange()
        differences = [field for field, value in before.items() if field not in SUBMISSION_KEYS and verified.get(field) != (after[field] if field in TARGETS else value)]
        if differences:
            raise RuntimeError("action readback differs in fields: " + ",".join(differences))
    except Exception:
        restored = False
        try:
            client.exchange({**before, "ReturnPage": "/action.htm", "DefaultSubmit": "Apply"})
            readback = client.exchange()
            restored = all(value == readback.get(field) for field, value in before.items() if field not in SUBMISSION_KEYS)
        finally:
            print(json.dumps({"rollback_attempted": True, "restored": restored}))
        raise
    return {**plan, "verified": True, "other_fields_unchanged": True,
            "encrypted_rollback": str(rollback), "real_phone_call_placed": False}


def main() -> int:
    """Expose a read-only plan by default and an explicit two-field apply operation."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--old-port", type=int, required=True)
    parser.add_argument("--new-port", type=int, required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not (1 <= args.old_port <= 65535 and 1 <= args.new_port <= 65535) or args.old_port == args.new_port:
        parser.error("ports must be different positive integers in 1..65535")
    result = migrate(Client(args.config.resolve()), args.old_port, args.new_port, args.apply)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
