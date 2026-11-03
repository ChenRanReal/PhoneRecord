"""Fanvil 话机 Web 客户端（登录 / 读配置 / 改配置）。

话机的登录是 challenge 模式：
    GET /                -> Set-Cookie: auth=<nonce>
    GET /key==nonce      -> 返回 nonce 明文
    POST /  encoded=<user>:md5(user:pass:nonce)

设备对请求频率敏感、偶发 RST，因此所有请求串行并带退避重试。
它的 web server 声明了 chunked 但实际不是，这里做了容错清洗。

命令行：
    python tools/phone_client.py show
    python tools/phone_client.py set-record --server-ip <PC_IP> --port 10000 --codec PCMA
"""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import socket
import sys
import time
from pathlib import Path
from urllib.parse import urlencode

HOST = os.environ.get("PHONE_RECORD_PHONE_IP", "")
PORT = 80
USER = os.environ.get("PHONE_RECORD_PHONE_USER", "")
PASSWORD = os.environ.get("PHONE_RECORD_PHONE_PASSWORD", "")

COOKIE_FILE = Path(__file__).resolve().parent.parent / ".phone_cookie"


# --------------------------------------------------------------------------- #
# 传输层
# --------------------------------------------------------------------------- #


class PhoneClient:
    def __init__(
        self,
        host: str | None = None,
        port: int | None = None,
        user: str | None = None,
        password: str | None = None,
    ) -> None:
        """Use explicit arguments or operator environment values; never guess a phone."""
        self.host = host if host is not None else os.environ.get("PHONE_RECORD_PHONE_IP", "")
        self.port = port if port is not None else int(os.environ.get("PHONE_RECORD_PHONE_PORT", "80"))
        self.user = user if user is not None else os.environ.get("PHONE_RECORD_PHONE_USER", "")
        self.password = password if password is not None else os.environ.get("PHONE_RECORD_PHONE_PASSWORD", "")
        if not self.host.strip() or not self.user.strip():
            raise ValueError("Set PHONE_RECORD_PHONE_IP and PHONE_RECORD_PHONE_USER, or pass host/user explicitly")
        if not 1 <= self.port <= 65535:
            raise ValueError("Phone web port must be an integer from 1 to 65535")
        self.cookie = ""
        self.last_status = ""
        self._load_cookie()

    # -- cookie ------------------------------------------------------------ #

    def _load_cookie(self) -> None:
        try:
            self.cookie = COOKIE_FILE.read_text(encoding="utf-8").strip()
        except OSError:
            self.cookie = ""

    def _save_cookie(self) -> None:
        try:
            COOKIE_FILE.write_text(self.cookie, encoding="utf-8")
        except OSError:
            pass

    # -- 原始收发 ---------------------------------------------------------- #

    def _raw(self, payload: bytes, timeout: float = 10.0, retries: int = 3) -> bytes:
        last: bytes = b""
        for attempt in range(retries):
            try:
                with socket.create_connection((self.host, self.port), timeout=timeout) as sock:
                    sock.sendall(payload)
                    chunks: list[bytes] = []
                    try:
                        while True:
                            data = sock.recv(65536)
                            if not data:
                                break
                            chunks.append(data)
                    except socket.timeout:
                        pass
                    result = b"".join(chunks)
                    if result:
                        return result
            except OSError as exc:
                last = str(exc).encode()
            time.sleep(0.6 * (attempt + 1))
        return last

    @staticmethod
    def _split(response: bytes) -> tuple[str, bytes]:
        if b"\r\n\r\n" in response:
            head, body = response.split(b"\r\n\r\n", 1)
        else:
            head, body = response, b""
        return head.decode("latin-1"), body

    @staticmethod
    def _header(head: str, name: str) -> str:
        match = re.search(r"^%s:\s*(.+)$" % name, head, re.I | re.M)
        return match.group(1).strip() if match else ""

    @staticmethod
    def _clean_body(body: bytes, head: str) -> bytes:
        if "chunked" not in head.lower():
            return body
        cleaned = re.sub(rb"\r\n[0-9a-fA-F]{1,4}\r\n", b"\r\n", body)
        cleaned = re.sub(rb"^[0-9a-fA-F]{1,4}\r\n", b"", cleaned)
        cleaned = re.sub(rb"\r\n[0-9a-fA-F]{1,4}$", b"", cleaned)
        return cleaned

    # -- 登录 -------------------------------------------------------------- #

    def login(self, attempts: int = 4) -> bool:
        for attempt in range(attempts):
            try:
                return self._login_once()
            except RuntimeError:
                if attempt == attempts - 1:
                    raise
                wait = 2.0 * (attempt + 1)
                print(f"   登录被限流，{wait:.0f}s 后重试...")
                time.sleep(wait)
        return False

    def _login_once(self) -> bool:
        head, body = self._split(
            self._raw(
                b"GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\n"
                b"Connection: close\r\n\r\n" % self.host.encode()
            )
        )
        page = body.decode("utf-8", "ignore")

        # 设备会按源 IP 记住会话，此时直接返回主页面而非登录页
        if "logon_content" not in page:
            cookie = self._header(head, "Set-Cookie")
            if "auth=" in cookie:
                self.cookie = cookie.split("auth=")[-1].split(";")[0]
                self._save_cookie()
            return True

        cookie = self._header(head, "Set-Cookie").split("auth=")[-1].split(";")[0]
        if not cookie:
            cookie = self.cookie
        if not cookie:
            raise RuntimeError("拿不到 nonce，设备可能限流，稍后重试")

        head, body = self._split(
            self._raw(
                (
                    "GET /key==nonce?now=%d HTTP/1.1\r\nHost: %s\r\n"
                    "Referer: http://%s/\r\nCookie: auth=%s\r\nConnection: close\r\n\r\n"
                    % (int(time.time() * 1000), self.host, self.host, cookie)
                ).encode()
            )
        )
        nonce = body.decode("latin-1").strip()
        if not nonce:
            raise RuntimeError("拿不到 nonce，设备可能限流，稍后重试")

        digest = hashlib.md5(
            ("%s:%s:%s" % (self.user, self.password, nonce)).encode()
        ).hexdigest()
        form = urlencode(
            {
                "encoded": "%s:%s" % (self.user, digest),
                "CurLanguage": "cn",
                "ReturnPage": "/",
            }
        )
        head, body = self._split(
            self._raw(
                (
                    "POST / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "Content-Length: %d\r\nReferer: http://%s/\r\n"
                    "Cookie: auth=%s\r\nConnection: close\r\n\r\n%s"
                    % (self.host, len(form), self.host, cookie, form)
                ).encode()
            )
        )
        set_cookie = self._header(head, "Set-Cookie")
        if set_cookie and "auth=" in set_cookie:
            cookie = set_cookie.split("auth=")[-1].split(";")[0]

        self.cookie = cookie
        self._save_cookie()
        return b"logon_content" not in body

    # -- 页面 -------------------------------------------------------------- #

    def _cookie_header(self) -> str:
        return f"Cookie: auth={self.cookie}\r\n" if self.cookie else ""

    def get(self, path: str, tries: int = 3) -> tuple[str, str]:
        """返回 (状态行, 正文)。失败会尝试重新登录。"""
        status = ""
        body = b""
        for attempt in range(tries):
            response = self._raw(
                (
                    "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\n"
                    "Accept: */*\r\nReferer: http://%s/\r\n%s"
                    "Connection: close\r\n\r\n"
                    % (path, self.host, self.host, self._cookie_header())
                ).encode()
            )
            head, raw_body = self._split(response)
            status = head.split("\r\n")[0]
            set_cookie = self._header(head, "Set-Cookie")
            if set_cookie and "auth=" in set_cookie:
                fresh = set_cookie.split("auth=")[-1].split(";")[0]
                if len(fresh) >= 8:
                    self.cookie = fresh
                    self._save_cookie()

            body = self._clean_body(raw_body, head)
            text = body.decode("utf-8", "ignore")
            if "404" not in status and "logon_content" not in text:
                time.sleep(0.35)
                return status, text

            time.sleep(0.8)
            if attempt == 1:
                self.login()

        return status, body.decode("utf-8", "ignore")

    def post(self, path: str, fields: dict[str, str]) -> tuple[str, str]:
        """Submit one form while logging metadata rather than cookies or request content."""
        form = urlencode(fields)
        payload = (
            "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %d\r\nReferer: http://%s%s\r\n%s"
            "Connection: close\r\n\r\n%s"
            % (path, self.host, len(form.encode()), self.host, path,
               self._cookie_header(), form)
        ).encode()
        logging.getLogger("phone").info(
            "HTTP_TX method=POST path=%s bytes=%d cookie_present=%s",
            path, len(payload), bool(self.cookie),
        )
        response = self._raw(payload)
        head, raw_body = self._split(response)
        status = head.split("\r\n")[0]
        body = self._clean_body(raw_body, head).decode("utf-8", "ignore")
        time.sleep(0.35)
        return status, body


# --------------------------------------------------------------------------- #
# 表单解析
# --------------------------------------------------------------------------- #


def parse_inputs(html: str) -> dict[str, dict[str, str | bool]]:
    """提取所有具名 input 的当前值。"""
    result: dict[str, dict[str, str | bool]] = {}
    for tag in re.finditer(r"<input\b([^>]*)>", html, re.I):
        attrs = tag.group(1)
        name_match = re.search(r'name\s*=\s*["\']([^"\']+)["\']', attrs, re.I)
        if not name_match:
            continue
        name = name_match.group(1)
        type_match = re.search(r'type\s*=\s*["\']([^"\']+)["\']', attrs, re.I)
        value_match = re.search(r'value\s*=\s*["\']([^"\']*)["\']', attrs, re.I)
        result[name] = {
            "type": (type_match.group(1).lower() if type_match else "text"),
            "value": value_match.group(1) if value_match else "",
            "checked": bool(re.search(r"\bCHECKED\b", attrs, re.I)),
        }
    return result


def parse_selects(html: str) -> dict[str, dict[str, object]]:
    """提取所有 select 的选项与选中项。"""
    result: dict[str, dict[str, object]] = {}
    for select in re.finditer(r"<select\b([^>]*)>(.*?)</select>", html, re.I | re.S):
        name_match = re.search(r'name\s*=\s*["\']([^"\']+)["\']', select.group(1), re.I)
        if not name_match:
            continue
        name = name_match.group(1)
        options: list[tuple[str, str, bool]] = []
        for option in re.finditer(r"<option\b([^>]*)>(.*?)</option>", select.group(2), re.I | re.S):
            attrs = option.group(1)
            value_match = re.search(r'value\s*=\s*["\']([^"\']*)["\']', attrs, re.I)
            label = re.sub(r"<[^>]+>", "", option.group(2)).strip()
            options.append(
                (
                    value_match.group(1) if value_match else "",
                    label,
                    bool(re.search(r"\bSELECTED\b", attrs, re.I)),
                )
            )
        result[name] = {
            "options": options,
            "selected": next((opt[0] for opt in options if opt[2]), None),
        }
    return result


# --------------------------------------------------------------------------- #
# 业务动作
# --------------------------------------------------------------------------- #


def show_record(client: PhoneClient) -> None:
    status, html = client.get("/record.htm")
    print(f"状态: {status}")
    inputs = parse_inputs(html)
    selects = parse_selects(html)

    print("\n[开关]")
    for name in ("EnableRecord_RW",):
        if name in inputs:
            print(f"  {name} = {inputs[name]['value']!r} checked={inputs[name]['checked']}")

    print("\n[下拉]")
    for name in ("CSE_RecordType_RW", "CSE_RecordVoiceCodec_R"):
        if name in selects:
            options = selects[name]["options"]
            print(f"  {name} 当前={selects[name]['selected']!r}")
            for value, label, selected in options:
                mark = " *" if selected else "  "
                print(f"    {mark} {value} = {label}")

    print("\n[文本]")
    for name in ("SYS_RecordAddr_RW", "SYS_RecordPort_RW", "Record_NumTabLen_R"):
        if name in inputs:
            print(f"  {name} = {inputs[name]['value']!r}")


def set_record(
    client: PhoneClient, server_ip: str, server_port: int, codec: str, enable: bool = True
) -> None:
    fields = {
        "EnableRecord_RW": "ON" if enable else "",
        "CSE_RecordType_RW": "1",
        "SYS_RecordAddr_RW": server_ip,
        "SYS_RecordPort_RW": str(server_port),
        "CheckBoxManager": "EnableRecord_RW,CSE_EnablePbxRec_RW",
        "DefaultSubmit": "Apply",
        "ReturnPage": "/record.htm",
        "CurLanguage": "cn",
    }
    status, body = client.post("/record.htm", fields)
    print(f"提交状态: {status}")

    if "401" in status or "Not Authorized" in body:
        print("!! 权限不足：guest 账号无法修改录音配置，需要 admin")
        return

    time.sleep(1.0)
    show_record(client)


def main() -> int:
    """Read an explicit target and keep session credentials out of CLI output."""
    parser = argparse.ArgumentParser(description="Fanvil 话机 Web 工具")
    parser.add_argument("action", choices=["show", "login", "set-record"])
    parser.add_argument("--host", help="Phone host; defaults to PHONE_RECORD_PHONE_IP")
    parser.add_argument("--user", help="Web user; defaults to PHONE_RECORD_PHONE_USER")
    parser.add_argument("--password", help="Prefer the PHONE_RECORD_PHONE_PASSWORD environment value")
    parser.add_argument("--server-ip", help="Required recording PC IPv4 for set-record")
    parser.add_argument("--web-port", type=int, help="Phone HTTP port; defaults to PHONE_RECORD_PHONE_PORT or 80")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--codec", default="PCMA")
    args = parser.parse_args()

    if args.action == "set-record" and not args.server_ip:
        parser.error("--server-ip is required for set-record")
    try:
        client = PhoneClient(host=args.host, port=args.web_port, user=args.user, password=args.password)
    except ValueError as error:
        parser.error(str(error))
    if not client.login():
        print("!! 登录失败")
        return 1
    print("登录成功（会话内容不显示）")

    if args.action == "login":
        return 0
    if args.action == "show":
        show_record(client)
        return 0
    if args.action == "set-record":
        set_record(client, args.server_ip, args.port, args.codec)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
