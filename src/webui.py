"""可视化管理界面（本地 Web）。

提供：
    /                 单页界面
    /api/calls        通话列表（支持方向、关键字过滤）
    /api/stats        汇总
    /api/unknown      未知号码（按出现次数排序）
    /api/contacts     新增/修改姓名
    /api/audio/<f>    录音流（支持 Range，浏览器可拖动进度）
    /api/reveal       在资源管理器里定位文件
    /api/sync         手动同步话机通话记录与号码目录
    /api/status       服务状态
"""

from __future__ import annotations

import json
import logging
import re
import subprocess
import sys
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse

LOGGER = logging.getLogger("webui")

WEB_DIR = Path(__file__).resolve().parent.parent / "web"
EXTRA_FILE = Path(__file__).resolve().parent.parent / "contacts_extra.json"


class UIHandler(BaseHTTPRequestHandler):
    server_version = "PhoneRecordUI/1.0"
    protocol_version = "HTTP/1.1"

    # -- 基础响应 ---------------------------------------------------------- #

    def _json(self, payload: dict | list, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path: Path, content_type: str) -> None:
        try:
            data = path.read_bytes()
        except OSError:
            self.send_error(404, "Not Found")
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt: str, *args) -> None:
        LOGGER.debug("UI " + fmt, *args)

    # -- 路由 -------------------------------------------------------------- #

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
        ctx = self.server.ctx  # type: ignore[attr-defined]

        if path in ("/", "/index.html"):
            self._file(WEB_DIR / "index.html", "text/html; charset=utf-8")
            return

        if path == "/api/calls":
            records = ctx.store.query(
                direction=query.get("dir", "all"),
                keyword=query.get("q", ""),
                limit=int(query.get("limit", 300)),
            )
            self._json([item.to_dict() for item in records])
            return

        if path == "/api/stats":
            self._json(ctx.store.stats())
            return

        if path == "/api/unknown":
            self._json(ctx.store.unknown_numbers())
            return

        if path == "/api/status":
            self._json(ctx.status())
            return

        if path.startswith("/api/audio/"):
            self._serve_audio(path[len("/api/audio/"):], ctx)
            return

        self.send_error(404, "Not Found")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            payload = json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError:
            payload = {}

        ctx = self.server.ctx  # type: ignore[attr-defined]

        if parsed.path == "/api/contacts":
            self._save_contact(payload, ctx)
            return

        if parsed.path == "/api/reveal":
            self._reveal(payload, ctx)
            return

        if parsed.path == "/api/sync":
            result = ctx.sync()
            self._json(result)
            return

        self.send_error(404, "Not Found")

    # -- 业务 -------------------------------------------------------------- #

    def _serve_audio(self, name: str, ctx) -> None:
        safe = re.sub(r"[^0-9A-Za-z._\-\u4e00-\u9fff]", "", name)
        path = (ctx.record_dir / safe).resolve()
        if not str(path).startswith(str(ctx.record_dir.resolve())) or not path.exists():
            self.send_error(404, "Not Found")
            return

        size = path.stat().st_size
        start, end = 0, size - 1

        range_header = self.headers.get("Range")
        if range_header:
            match = re.match(r"bytes=(\d*)-(\d*)", range_header)
            if match:
                if match.group(1):
                    start = int(match.group(1))
                if match.group(2):
                    end = min(int(match.group(2)), size - 1)
                if start > end:
                    self.send_error(416, "Range Not Satisfiable")
                    return

        length = end - start + 1
        self.send_response(206 if range_header else 200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if range_header:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        with path.open("rb") as handle:
            handle.seek(start)
            remaining = length
            while remaining > 0:
                chunk = handle.read(min(65536, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except OSError:
                    break
                remaining -= len(chunk)

    def _save_contact(self, payload: dict, ctx) -> None:
        number = str(payload.get("number", "")).strip()
        name = str(payload.get("name", "")).strip()
        if not number or not name:
            self._json({"ok": False, "error": "号码和姓名都不能为空"}, 400)
            return

        try:
            extra = json.loads(EXTRA_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            extra = {}

        extra[number] = name
        EXTRA_FILE.write_text(
            json.dumps(extra, ensure_ascii=False, indent=2), encoding="utf-8"
        )

        ctx.store.load_names()
        ctx.store.rebuild()
        LOGGER.info("通讯录新增 %s -> %s", number, name)
        self._json({"ok": True, "number": number, "name": name})

    def _reveal(self, payload: dict, ctx) -> None:
        name = str(payload.get("file", "")).strip()
        if not name:
            self._json({"ok": False, "error": "缺少文件名"}, 400)
            return

        safe = re.sub(r"[^0-9A-Za-z._\-\u4e00-\u9fff]", "", name)
        path = (ctx.record_dir / safe).resolve()
        if not path.exists():
            self._json({"ok": False, "error": "文件不存在"}, 404)
            return

        try:
            subprocess.Popen(["explorer", "/select,", str(path)])
            self._json({"ok": True, "path": str(path)})
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("定位文件失败")
            self._json({"ok": False, "error": str(exc)}, 500)


class WebUI:
    def __init__(self, ctx, host: str = "0.0.0.0", port: int = 8090) -> None:
        self.ctx = ctx
        self.host = host
        self.port = port
        self._server: ThreadingHTTPServer | None = None

    def serve_forever(self) -> None:
        self._server = ThreadingHTTPServer((self.host, self.port), UIHandler)
        self._server.ctx = self.ctx  # type: ignore[attr-defined]
        LOGGER.info("管理界面 http://%s:%d/", self.host, self.port)
        self._server.serve_forever()

    def shutdown(self) -> None:
        if self._server:
            self._server.shutdown()
            self._server.server_close()
