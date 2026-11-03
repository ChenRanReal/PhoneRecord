"""Action URL 协调服务。

X3S 自身不会自动开始录音，也没有可配置的录音键。因此用话机的 Action URL
事件回调作为触发源：

    话机 --Call Established--> 本机:8080/event?ev=est  --> 反向 POST 话机 起录
    话机 --Call Terminated --> 本机:8080/event?ev=end  --> 反向 POST 话机 停录

起录入口有两个，判据只有一个：服务器有没有真的收到控制连接与 RTP。

    /Start        固件自身 JS 用的动作 CGI。xmlUtil.js 里 xAjax 把 url 原样交给
                  XMLHttpRequest.open()，页面中的 "Start" 因此解析成 /Start。
                  空载 POST 返回一页 "Processing..."（5817 字节），是异步动作；
                  而同名 GET 回 404 —— 页面路由不会这样分流。
    /record.htm   页面端点（表单提交），只回 860 字节的跳回壳页。10:34 与 10:45
                  两次成功都是打这个。

两者都出现过「200 OK 但零推流」，所以按 /Start → 等 3 秒校验 → /record.htm
的顺序升级重试；校验靠服务器侧的录音会话数（Recorder.active_sessions，由
app.py 注入）。Stop 则两个入口都发一遍（未在录音时为无害空操作）。

录音数据仍由 record_server.py 通过 UDP 接收，本服务只负责"按一下开始/停止"。

用法：
    python src/call_hook.py
    python src/call_hook.py --port 8080
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from phone_client import HOST, PhoneClient

LOGGER = logging.getLogger("hook")

ESTABLISHED_EVENTS = {"est", "established", "talking", "answer"}
TERMINATED_EVENTS = {"end", "terminated", "hangup", "release"}


class HookHandler(BaseHTTPRequestHandler):
    server_version = "PhoneRecordHook/1.0"
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        self._dispatch("GET")

    def do_POST(self) -> None:
        self._dispatch("POST")

    def _dispatch(self, method: str) -> None:
        parsed = urlparse(self.path)
        params: dict[str, str] = {
            key: values[0] for key, values in parse_qs(parsed.query).items()
        }

        if method == "POST":
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                length = 0
            if length:
                body = self.rfile.read(length).decode("utf-8", "ignore")
                params.update(
                    {key: values[0] for key, values in parse_qs(body).items()}
                )

        # 先回 200，避免话机因为等待而重试或超时
        payload = b"OK"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

        event = (params.get("ev") or params.get("event") or "").lower()
        hook: Hook = self.server.hook  # type: ignore[attr-defined]
        threading.Thread(
            target=hook.handle, args=(event, params), daemon=True
        ).start()

    def log_message(self, fmt: str, *args) -> None:
        LOGGER.debug("HTTP " + fmt, *args)


class Hook:
    """把话机事件翻译成录音开始/停止指令。"""

    #: 起录入口，按优先级排列：先固件自己的动作 CGI，再页面端点
    START_ENDPOINTS = ("/Start", "/record.htm")
    #: 停录入口，两个都发一遍
    STOP_ENDPOINTS = ("/Stop", "/record.htm")
    #: 发出 Start 后等多久再问服务器有没有收到推流（秒）
    STREAM_CHECK_DELAY = 3.0
    #: 换下一个入口前，先把可能已置位的状态复位（秒）
    RESET_DELAY = 1.2
    #: 话机繁忙（503 Server Too Busy）时的重试次数与退避基数（秒）
    BUSY_RETRIES = 3
    BUSY_BACKOFF = 1.5

    def __init__(
        self,
        phone_host: str,
        http_port: int,
        active_sessions=None,
    ) -> None:
        self.phone_host = phone_host
        self.http_port = http_port
        self.phone = PhoneClient(host=phone_host)
        self._active_sessions = active_sessions
        self._lock = threading.Lock()
        self._recording = False
        self._endpoint = ""
        self._last_event = datetime.now()

    # -- 事件处理 ---------------------------------------------------------- #

    def handle(self, event: str, params: dict[str, str]) -> None:
        with self._lock:
            self._last_event = datetime.now()
            who = params.get("user") or params.get("line") or "?"
            if event in ESTABLISHED_EVENTS or event == "":
                self._start(who, params)
            elif event in TERMINATED_EVENTS:
                self._stop(who, params)
            else:
                LOGGER.info("忽略事件 ev=%s 参数=%s", event, params)

    def _start(self, who: str, params: dict[str, str]) -> None:
        if self._recording:
            LOGGER.info("已在录音中，忽略重复的开始事件（%s）", who)
            return

        LOGGER.info("通话建立 -> 请求话机开始录音（%s）", who)
        self._clear_stale_state()
        for index, path in enumerate(self.START_ENDPOINTS):
            if index:
                # 上一个入口可能已经把状态机置位，先复位再试下一个
                self._stop_now("/Stop")
                time.sleep(self.RESET_DELAY)

            ok, status, body = self._post(path, "Start")
            if not ok:
                LOGGER.warning(
                    "起录入口 %s 未被接受（%s，正文 %d 字节）",
                    path,
                    status or "无响应",
                    len(body or ""),
                )
                continue

            LOGGER.info(
                "起录入口 %s 返回 %s，等 %.1fs 看服务器是否收到推流",
                path,
                status,
                self.STREAM_CHECK_DELAY,
            )
            time.sleep(self.STREAM_CHECK_DELAY)

            sessions = self._sessions()
            if sessions is None:
                LOGGER.info(
                    "读不到录音会话数，只能按 200 认为成功（入口 %s）", path
                )
                self._recording = True
                self._endpoint = path
                return
            if sessions > 0:
                LOGGER.info("推流已建立（入口 %s，会话 %d）", path, sessions)
                self._recording = True
                self._endpoint = path
                return
            LOGGER.warning(
                "入口 %s 返回成功但服务器零推流，说明话机只动了本地状态机",
                path,
            )

        # 全部入口都没建起推流：复位后放弃，等下一通再试
        self._stop_now("/Stop")
        self._recording = False
        self._endpoint = ""
        LOGGER.error("所有起录入口都未建立推流，已发 Stop 复位状态机")

    # -- 起录前置守卫 ------------------------------------------------------ #

    def _clear_stale_state(self) -> None:
        """起录前确认话机录音状态机是空闲(4)。

        未被收尾的会话（服务被强杀、Stop 没发出去）会让 recordStatus 卡在 1。
        此后每一次 Start 都被当成重复请求：LCD 图标照样点亮、HTTP 照样 200，
        但不建立控制连接 —— 与「200 但零推流」的症状完全一致，事后无法区分。
        所以每次起录前先读一次状态，非 4 就先发 Stop 复位。
        """
        state = self._record_state()
        LOGGER.info("起录前话机 recordStatus=%s（4=空闲）", state)
        if state is None or state == "4":
            return
        LOGGER.warning("话机录音状态机不是空闲（%s），先发 Stop 复位", state)
        self._post("/Stop", "Stop")
        time.sleep(2.0)
        LOGGER.info("复位后 recordStatus=%s", self._record_state())

    def _record_state(self):
        """读话机 recordStatus；取不到返回 None（不阻塞起录）。"""
        try:
            status, page = self.phone.get("/record.htm")
        except Exception:
            LOGGER.exception("读取话机录音状态失败")
            return None
        text = page or ""
        if "logon_content" in text:
            LOGGER.info("读状态拿到登录页（%s），会话可能已失效", status)
            return None
        key = text.find("recordStatus")
        if key < 0:
            return None
        head = text[key:]
        first = head.find('"')
        second = head.find('"', first + 1) if first >= 0 else -1
        if first < 0 or second < 0:
            return None
        digits = "".join(ch for ch in head[first + 1:second] if ch.isdigit())
        return digits or None

    def _stop(self, who: str, params: dict[str, str]) -> None:
        if not self._recording:
            LOGGER.info("当前未在录音，忽略结束事件（%s）", who)
            return
        LOGGER.info(
            "通话结束 -> 请求话机停止录音（%s，入口 %s）",
            who,
            self._endpoint or "?",
        )
        # 停录固定先走 /Stop 动作 CGI：起录可能走了 /Start，往 /Start 发 Stop 是错入口
        self._stop_now("/Stop")
        self._recording = False
        self._endpoint = ""

    def _stop_now(self, first: str) -> None:
        order = [first] + [p for p in self.STOP_ENDPOINTS if p != first]
        for path in order:
            ok, status, _ = self._post(path, "Stop")
            LOGGER.debug(
                "停录 %s -> %s（%s）",
                path,
                status or "无响应",
                "已接受" if ok else "未接受",
            )

    # -- 与话机交互 -------------------------------------------------------- #

    def _post(self, path: str, action: str):
        """向指定入口发一条 DefaultRecord 指令，返回 (是否被接受, 状态行, 正文)。"""
        fields = {"DefaultRecord": action}
        if path == "/record.htm":
            # 页面端点做表单提交时带 ReturnPage；动作 CGI 不带
            fields["ReturnPage"] = "/record.htm"
        try:
            status, body = self.phone.post(path, fields)
            # 话机 web 服务忙时会回 503 Server Too Busy（15:49:46 实测到），
            # 这是暂时性拒绝：既不等于「已接受」，也不该立刻放弃。
            for attempt in range(self.BUSY_RETRIES):
                if "503" not in status:
                    break
                wait = self.BUSY_BACKOFF * (attempt + 1)
                LOGGER.info(
                    "话机繁忙（%s），%.1fs 后重试 %s（第 %d 次）",
                    status or "无响应",
                    wait,
                    path,
                    attempt + 1,
                )
                time.sleep(wait)
                status, body = self.phone.post(path, fields)
            if self._session_dead(status, body):
                LOGGER.info("会话失效（登录页/401），重新登录话机后重发 %s", path)
                try:
                    self.phone.login()
                except Exception:
                    LOGGER.exception("重新登录话机失败")
                status, body = self.phone.post(path, fields)
            LOGGER.debug(
                "%s %s -> %s，正文 %d 字节", action, path, status, len(body or "")
            )
            accepted = "200" in status and not self._session_dead(status, body)
            return accepted, status, body
        except Exception:
            LOGGER.exception("发送 %s 到 %s 失败", action, path)
            return False, "", ""

    def _sessions(self):
        """服务器当前录音会话数；取不到返回 None（调用方退回按 200 判定）。"""
        if self._active_sessions is None:
            return None
        try:
            value = self._active_sessions
            value = value() if callable(value) else value
            return int(value)
        except Exception:
            LOGGER.exception("读取录音会话数失败")
            return None

    @staticmethod
    def _session_dead(status: str, body: str) -> bool:
        """话机会话失效时返回的是登录页 + 200 OK，必须认 logon_content。

        只判 401 / Not Authorized 会把「登录页 200」当成成功：日志写
        已接受录音指令，而话机其实什么都没做（假成功通道）。
        """
        text = body or ""
        return (
            "401" in status
            or "Not Authorized" in text
            or "logon_content" in text
            or "User Name or Password Error" in text
        )


def setup_logging(log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    log_file = log_dir / "hook.log"

    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(message)s", "%H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.DEBUG)

    console = logging.StreamHandler(sys.stdout)
    console.setLevel(logging.INFO)
    console.setFormatter(fmt)

    handler = TimedRotatingFileHandler(
        log_file, when="midnight", backupCount=30, encoding="utf-8"
    )
    handler.setLevel(logging.DEBUG)
    handler.setFormatter(fmt)

    root.addHandler(console)
    root.addHandler(handler)


def main() -> int:
    """Start the optional Python event listener for an explicitly configured phone."""
    parser = argparse.ArgumentParser(description="话机 Action URL 协调服务")
    parser.add_argument("--port", type=int, default=8080, help="HTTP 监听端口")
    parser.add_argument("--phone", default=HOST, help="Phone address; defaults to PHONE_RECORD_PHONE_IP")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    base = Path(__file__).resolve().parent.parent
    cfg_path = base / "config.json"
    log_dir = base / "logs"
    if cfg_path.exists():
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        log_dir = Path(cfg.get("log_dir", "logs"))
        if not log_dir.is_absolute():
            log_dir = base / log_dir

    setup_logging(log_dir)
    if args.verbose:
        logging.getLogger().handlers[0].setLevel(logging.DEBUG)

    try:
        hook = Hook(phone_host=args.phone, http_port=args.port)
    except ValueError as error:
        parser.error(str(error))
    server = ThreadingHTTPServer(("0.0.0.0", args.port), HookHandler)
    server.hook = hook  # type: ignore[attr-defined]

    LOGGER.info("协调服务监听 0.0.0.0:%d", args.port)
    LOGGER.info("等待话机 Action URL 回调：/event?ev=est|end")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        LOGGER.info("收到中断，退出")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
