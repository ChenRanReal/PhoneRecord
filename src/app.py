"""PhoneRecord 统一入口。

一个进程内跑三个监听：

    TCP 10000 / UDP 20000+   录音通道      record_server.Recorder
    HTTP 8080                事件协调      call_hook.Hook
    HTTP 8090                管理界面      webui.WebUI

另有后台线程每 5 分钟同步一次话机的通话记录与号码目录。

用法：
    python src/app.py
    pythonw src/app.py        # 无控制台窗口（开机自启用）
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import threading
import time
from datetime import datetime
from http.server import ThreadingHTTPServer
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE / "src"))
sys.path.insert(0, str(BASE / "tools"))

from call_hook import Hook, HookHandler
from callstore import CallStore, parse_calllog
from phone_client import PhoneClient
from record_server import Config, Recorder, setup_logging
from webui import EXTRA_FILE, WebUI

LOGGER = logging.getLogger("app")

#: 同步周期。300 秒意味着每 5 分钟就登录一次话机并拉 117KB calllog，
#: 而设备按源 IP 记单一会话、对连续请求极敏感 —— 15:49:46 实测到它在一次
#: 同步后 20 秒直接回 503 拒收起录指令。放宽到 15 分钟。
SYNC_INTERVAL = 1800  # 30min：过频同步会登录话机消耗请求配额，触发限流（15:49 实测 Start 被 503）


class AppContext:
    """WebUI 的数据与状态来源。"""

    def __init__(self, cfg: Config, recorder: Recorder) -> None:
        self.cfg = cfg
        self.recorder = recorder
        self.record_dir = Path(cfg.record_dir)
        self.store = CallStore(
            record_dir=self.record_dir,
            ledger=Path(cfg.ledger_file),
            directory_file=Path(cfg.directory_file),
            extra_file=EXTRA_FILE,
        )
        self._started_at = datetime.now()
        self._sync_lock = threading.Lock()

    # -- 状态 -------------------------------------------------------------- #

    def status(self) -> dict:
        return {
            "healthy": True,
            "recording": self.recorder.active_sessions,
            "uptime": str(datetime.now() - self._started_at).split(".")[0],
            "message": "",
        }

    # -- 同步 -------------------------------------------------------------- #

    def sync(self) -> dict:
        """Read call metadata from the phone selected by the operator environment."""
        if not self._sync_lock.acquire(blocking=False):
            return {"ok": False, "error": "同步正在进行中"}
        try:
            if self.recorder.active_sessions > 0:
                # 通话中绝不碰话机 web：一次同步是登录 + 117KB calllog，
                # 足以把话机 web 服务打到 503，正好毁掉同时下发的起录指令。
                return {"ok": False, "error": "录音进行中，跳过同步"}
            client = PhoneClient()
            if not client.login():
                return {"ok": False, "error": "登录话机失败"}

            stamp = int(time.time() * 1000)
            _, calllog_xml = client.get(f"/calllog.xml?now={stamp}")
            records = parse_calllog(calllog_xml)
            if records:
                self.store.update_calllog(records)

            self.store.load_names()
            self.store.rebuild()

            LOGGER.info("已同步话机通话记录 %d 条", len(records))
            return {"ok": True, "calls": len(records)}
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("同步失败")
            return {"ok": False, "error": str(exc)}
        finally:
            self._sync_lock.release()


def sync_loop(ctx: AppContext, first_delay: float = 3.0) -> None:
    time.sleep(first_delay)
    while True:
        try:
            ctx.sync()
        except Exception:  # noqa: BLE001
            LOGGER.exception("同步线程异常")
        time.sleep(SYNC_INTERVAL)


def start_hook_server(hook: Hook, port: int) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("0.0.0.0", port), HookHandler)
    server.hook = hook  # type: ignore[attr-defined]
    threading.Thread(target=server.serve_forever, daemon=True, name="hook").start()
    LOGGER.info("事件协调监听 0.0.0.0:%d", port)
    return server


def main() -> int:
    """Validate operator settings before starting the optional Python services."""
    parser = argparse.ArgumentParser(description="PhoneRecord 统一服务")
    parser.add_argument("--config")
    parser.add_argument("--ui-port", type=int)
    parser.add_argument("--hook-port", type=int)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    cfg = Config.load(args.config)
    if not cfg.advertise_ip:
        parser.error("Set advertise_ip in your local config.json before starting")
    try:
        phone = PhoneClient()
    except ValueError as error:
        parser.error(str(error))
    ui_port = args.ui_port or cfg.ui_port
    hook_port = args.hook_port or cfg.hook_port
    setup_logging(cfg)
    if args.verbose:
        logging.getLogger().handlers[0].setLevel(logging.DEBUG)

    LOGGER.info("=" * 52)
    LOGGER.info("PhoneRecord 启动")
    LOGGER.info("录音目录 %s", cfg.record_dir)

    recorder = Recorder(cfg)
    ctx = AppContext(cfg, recorder)

    threading.Thread(
        target=recorder.serve_forever, daemon=True, name="recorder"
    ).start()

    # 把录音会话数交给 hook：判据不是 HTTP 200，而是服务器真的收到了推流
    hook = Hook(
        phone_host=phone.host,
        http_port=hook_port,
        active_sessions=lambda: recorder.active_sessions,
    )
    hook_server = start_hook_server(hook, hook_port)

    threading.Thread(target=sync_loop, args=(ctx,), daemon=True, name="sync").start()

    ui = WebUI(ctx, host="0.0.0.0", port=ui_port)
    try:
        ui.serve_forever()
    except KeyboardInterrupt:
        LOGGER.info("收到中断，正在退出")
    finally:
        recorder.shutdown()
        hook_server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
