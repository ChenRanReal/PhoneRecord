"""Fanvil 服务器录音服务。

按 Fanvil《通话录音的配置及使用说明》3.2 节实现：

    话机 --TCP--> 本机:10000   控制指令（cmd=RecordStartRequest / RecordStopRequest）
    话机 --UDP--> 本机:动态端口  RTP（双向混音后的单流，PCMA/PCMU）
    本机落盘为 8kHz / 16bit / 单声道 WAV

用法：
    python src/record_server.py
    python src/record_server.py --config config.json
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import os
import re
import socket
import sys
import threading
import time
import wave
from dataclasses import dataclass
from datetime import datetime
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path
from urllib.parse import unquote

sys.path.insert(0, str(Path(__file__).resolve().parent))

import g711
import rtp

LOGGER = logging.getLogger("recorder")

SAMPLE_RATE = 8000
SAMPLE_WIDTH = 2
CHANNELS = 1


# --------------------------------------------------------------------------- #
# 配置
# --------------------------------------------------------------------------- #


@dataclass
class Config:
    bind_ip: str = "0.0.0.0"
    control_port: int = 10000
    advertise_ip: str = ""
    udp_port_start: int = 20000
    udp_port_end: int = 20100
    record_dir: str = "recordings"
    log_dir: str = "logs"
    directory_file: str = "directory.json"
    ledger_file: str = "recordings/calls.csv"
    default_codec: str = "PCMA"
    udp_idle_timeout: float = 5.0
    control_terminator: str = "\r\n"
    control_idle_frame_ms: int = 200
    ui_port: int = 8090
    hook_port: int = 8080

    @classmethod
    def load(cls, path: str | None) -> "Config":
        cfg = cls()
        if not path:
            default = Path(__file__).resolve().parent.parent / "config.json"
            path = str(default) if default.exists() else None
        if path and Path(path).exists():
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            for key, value in data.items():
                if hasattr(cfg, key):
                    setattr(cfg, key, value)
                else:
                    LOGGER.warning("配置项未知，已忽略: %s", key)

        # 相对路径统一挂到项目根，避免从其它目录启动时写错位置
        base = Path(__file__).resolve().parent.parent
        for attr in ("record_dir", "log_dir", "directory_file", "ledger_file"):
            value = Path(getattr(cfg, attr))
            if not value.is_absolute():
                setattr(cfg, attr, str(base / value))
        return cfg


# --------------------------------------------------------------------------- #
# 工具
# --------------------------------------------------------------------------- #


_ILLEGAL_PATH = re.compile(r'[\\/:*?"<>|\x00-\x1f]')


def sanitize(value: str, fallback: str = "unknown") -> str:
    """把号码/姓名清理成安全的文件名片段，保留中文。"""
    text = _ILLEGAL_PATH.sub("", (value or "").strip())
    text = re.sub(r"\s+", "", text)
    return text[:30] if text else fallback


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def parse_kv(text: str) -> dict[str, str]:
    """解析 Fanvil 的 & 分隔 kv 串，容忍空值与缺等号。"""
    result: dict[str, str] = {}
    for part in text.split("&"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key.strip()] = unquote(value.strip())
    return result


# --------------------------------------------------------------------------- #
# 号码 -> 姓名
# --------------------------------------------------------------------------- #


class Directory:
    """号码姓名目录；directory.json 被重新同步后自动重载。"""

    def __init__(self, path: str) -> None:
        self._path = Path(path)
        self._names: dict[str, str] = {}
        self._mtime = -1.0
        self._lock = threading.Lock()

    def _reload_if_changed(self) -> None:
        try:
            mtime = self._path.stat().st_mtime
        except OSError:
            return
        if mtime == self._mtime:
            return
        try:
            data = json.loads(self._path.read_text(encoding="utf-8"))
            self._names = data.get("names", {})
            self._mtime = mtime
            LOGGER.info("号码目录已载入，共 %d 个号码", len(self._names))
        except Exception:
            LOGGER.exception("读取号码目录失败: %s", self._path)

    def lookup(self, number: str) -> str:
        cleaned = re.sub(r"[^\d+]", "", number or "")
        if not cleaned:
            return ""
        with self._lock:
            self._reload_if_changed()
            return self._names.get(cleaned, "")


LEDGER_HEADER = [
    "开始时间", "结束时间", "时长秒", "方向", "本端", "对端号码",
    "对端姓名", "文件名", "包数", "丢包",
]
_LEDGER_LOCK = threading.Lock()


def append_ledger(path: str, row: list) -> None:
    """追加一行通话台账；首次创建时写入 BOM，方便 Excel 直接打开。"""
    target = Path(path)
    with _LEDGER_LOCK:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            with target.open("a", newline="", encoding="utf-8") as handle:
                csv.writer(handle).writerow(row)
        else:
            with target.open("w", newline="", encoding="utf-8-sig") as handle:
                writer = csv.writer(handle)
                writer.writerow(LEDGER_HEADER)
                writer.writerow(row)


# --------------------------------------------------------------------------- #
# UDP 端口池
# --------------------------------------------------------------------------- #


class PortAllocator:
    """在固定区间内分配 UDP 端口，绑定成功才算占用。"""

    def __init__(self, bind_ip: str, start: int, end: int) -> None:
        self._bind_ip = bind_ip
        self._start = start
        self._end = end
        self._used: set[int] = set()
        self._lock = threading.Lock()

    def acquire(self) -> tuple[int, socket.socket] | tuple[None, None]:
        with self._lock:
            for port in range(self._start, self._end + 1):
                if port in self._used:
                    continue
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try:
                    sock.bind((self._bind_ip, port))
                except OSError:
                    sock.close()
                    continue
                sock.settimeout(0.5)
                self._used.add(port)
                return port, sock
        return None, None

    def release(self, port: int) -> None:
        with self._lock:
            self._used.discard(port)


# --------------------------------------------------------------------------- #
# 单个录音会话
# --------------------------------------------------------------------------- #


@dataclass
class SessionStats:
    packets: int = 0
    bytes_pcm: int = 0
    first_seq: int | None = None
    last_seq: int | None = None
    out_of_order: int = 0
    peer: tuple[str, int] | None = None

    @property
    def duration(self) -> float:
        return self.bytes_pcm / (SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)

    @property
    def lost(self) -> int:
        if self.first_seq is None or self.last_seq is None:
            return 0
        expected = ((self.last_seq - self.first_seq) & 0xFFFF) + 1
        return max(0, expected - self.packets)


class RecordSession:
    """一次通话的录音会话：独占一个 UDP 端口，直到 Stop 或空闲超时。"""

    def __init__(
        self,
        session_id: str,
        params: dict[str, str],
        codec: str,
        port: int,
        sock: socket.socket,
        cfg: Config,
        on_finish,
        directory: Directory,
    ) -> None:
        self.session_id = session_id
        self.params = params
        self.codec = codec
        self.port = port
        self._sock = sock
        self._cfg = cfg
        self._on_finish = on_finish

        self.stats = SessionStats()
        self.started_at = time.time()
        self.ended_at: float | None = None
        self.file_path: Path | None = None

        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._wave: wave.Wave_write | None = None
        self._lock = threading.Lock()
        self._last_packet_at = 0.0

        self.local_number = params.get("local_number", "")
        self.remote_number = params.get("remote_number", "")
        self.direction = params.get("dir", "")
        # 话机上报的姓名来自它的 LDAP 云通讯录，最实时；没有就查本地目录
        self.peer_name = (
            params.get("remote_name", "").strip()
            or directory.lookup(self.remote_number)
        )

    # -- 生命周期 ---------------------------------------------------------- #

    def start(self) -> None:
        self._thread = threading.Thread(
            target=self._run, name=f"rec-{self.session_id}", daemon=True
        )
        self._thread.start()

    def stop(self, reason: str) -> None:
        if self._stop_event.is_set():
            return
        self._stop_event.set()
        LOGGER.info(
            "录音结束 id=%s 原因=%s 时长=%.1fs 包=%d 丢包=%d 文件=%s",
            self.session_id,
            reason,
            self.stats.duration,
            self.stats.packets,
            self.stats.lost,
            self.file_path.name if self.file_path else "(无)",
        )

    def join(self, timeout: float = 5.0) -> None:
        if self._thread:
            self._thread.join(timeout)

    # -- 接收循环 ---------------------------------------------------------- #

    def _run(self) -> None:
        try:
            self._socket_loop()
        except Exception:
            LOGGER.exception("录音会话异常 id=%s", self.session_id)
        finally:
            self._finalize()

    def _socket_loop(self) -> None:
        idle = self._cfg.udp_idle_timeout
        while not self._stop_event.is_set():
            try:
                packet, addr = self._sock.recvfrom(2048)
            except socket.timeout:
                if self.stats.packets == 0:
                    # 一直没等到 RTP：可能是话机已放弃本次录音
                    if time.time() - self.started_at > max(idle * 2, 10.0):
                        LOGGER.warning("会话 %s 未收到任何 RTP，放弃", self.session_id)
                        return
                    continue
                if time.time() - self._last_packet_at > idle:
                    self.stop("空闲超时")
                    return
                continue
            except OSError:
                return

            if self.stats.peer is None:
                self.stats.peer = addr
                LOGGER.debug("会话 %s 锁定 RTP 源 %s:%d", self.session_id, *addr)
            elif addr != self.stats.peer:
                continue

            self._last_packet_at = time.time()
            self._ingest(packet)

    def _ingest(self, packet: bytes) -> None:
        parsed = rtp.parse(packet)
        if parsed is None:
            return

        if parsed.payload_type not in (0, 8):
            LOGGER.debug("忽略非 G.711 载荷 PT=%d", parsed.payload_type)
            return

        with self._lock:
            if self.stats.first_seq is None:
                self.stats.first_seq = parsed.sequence
            else:
                gap = (parsed.sequence - self.stats.last_seq) & 0xFFFF
                if gap == 0 or gap > 0x8000:
                    self.stats.out_of_order += 1
            self.stats.last_seq = parsed.sequence
            self.stats.packets += 1

            pcm = g711.decode(parsed.payload, self.codec)
            self.stats.bytes_pcm += len(pcm)
            self._write(pcm)

    def _ensure_wave(self) -> wave.Wave_write:
        if self._wave is not None:
            return self._wave

        out_dir = Path(self._cfg.record_dir)
        out_dir.mkdir(parents=True, exist_ok=True)

        stamp = datetime.fromtimestamp(self.started_at).strftime("%Y%m%d-%H%M%S")
        parts = [
            stamp,
            sanitize(self.local_number, "local"),
            sanitize(self.remote_number, "remote"),
            sanitize(self.direction, "call"),
        ]
        if self.peer_name:
            parts.append(sanitize(self.peer_name, ""))
        name = "_".join(part for part in parts if part)
        path = out_dir / f"{name}.wav"
        index = 1
        while path.exists():
            path = out_dir / f"{name}_{index}.wav"
            index += 1

        handle = wave.open(str(path), "wb")
        handle.setnchannels(CHANNELS)
        handle.setsampwidth(SAMPLE_WIDTH)
        handle.setframerate(SAMPLE_RATE)
        self._wave = handle
        self.file_path = path
        LOGGER.info("开始写入 %s", path)
        return handle

    def _write(self, pcm: bytes) -> None:
        handle = self._ensure_wave()
        handle.writeframes(pcm)

    def _finalize(self) -> None:
        with self._lock:
            handle, self._wave = self._wave, None
        if handle is not None:
            try:
                handle.close()
            except Exception:
                LOGGER.exception("关闭 WAV 失败 %s", self.file_path)

        self.ended_at = time.time()

        if self.file_path is not None:
            try:
                append_ledger(
                    self._cfg.ledger_file,
                    [
                        datetime.fromtimestamp(self.started_at).strftime("%Y-%m-%d %H:%M:%S"),
                        datetime.fromtimestamp(self.ended_at).strftime("%Y-%m-%d %H:%M:%S"),
                        f"{self.stats.duration:.1f}",
                        self.direction,
                        self.local_number,
                        self.remote_number,
                        self.peer_name,
                        self.file_path.name,
                        self.stats.packets,
                        self.stats.lost,
                    ],
                )
            except Exception:
                LOGGER.exception("写台账失败")

        try:
            self._sock.close()
        except OSError:
            pass
        self._on_finish(self)


# --------------------------------------------------------------------------- #
# 控制通道
# --------------------------------------------------------------------------- #


class Recorder:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.directory = Directory(cfg.directory_file)
        self._ports = PortAllocator(cfg.bind_ip, cfg.udp_port_start, cfg.udp_port_end)
        self._sessions: dict[str, RecordSession] = {}
        self._lock = threading.Lock()
        self._tcp: socket.socket | None = None
        self._running = False

    # -- 会话管理 ---------------------------------------------------------- #

    @property
    def active_sessions(self) -> int:
        with self._lock:
            return len(self._sessions)

    def handle_start(self, params: dict[str, str]) -> tuple[bool, str]:
        session_id = params.get("id", "").strip()
        if not session_id:
            return False, "缺少 id"

        codec = (params.get("codec") or self.cfg.default_codec).upper()

        with self._lock:
            existing = self._sessions.get(session_id)
            if existing is not None:
                # 同一 id 重复 Start：沿用原端口，避免话机侧状态错乱
                LOGGER.warning("会话 %s 重复启动，复用端口 %d", session_id, existing.port)
                return True, ""

            port, sock = self._ports.acquire()
            if port is None:
                return False, "UDP 端口耗尽"

            session = RecordSession(
                session_id=session_id,
                params=params,
                codec=codec,
                port=port,
                sock=sock,
                cfg=self.cfg,
                on_finish=self._on_session_finished,
                directory=self.directory,
            )
            self._sessions[session_id] = session

        LOGGER.info(
            "录音开始 id=%s device=%s %s -> %s%s dir=%s codec=%s udp=%d",
            session_id,
            params.get("device", "?"),
            params.get("local_number", "?"),
            params.get("remote_number", "?"),
            f"({session.peer_name})" if session.peer_name else "",
            params.get("dir", "?"),
            codec,
            port,
        )
        session.start()
        return True, ""

    def handle_stop(self, params: dict[str, str]) -> None:
        session_id = params.get("id", "").strip()
        with self._lock:
            session = self._sessions.get(session_id)
        if session is None:
            LOGGER.warning("Stop 找不到会话 id=%s（可能已超时结束）", session_id)
            return
        session.stop(params.get("reason") or "话机请求")

    def _on_session_finished(self, session: RecordSession) -> None:
        with self._lock:
            self._sessions.pop(session.session_id, None)
        self._ports.release(session.port)

    # -- TCP 服务 ---------------------------------------------------------- #

    def serve_forever(self) -> None:
        self._tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._tcp.bind((self.cfg.bind_ip, self.cfg.control_port))
        self._tcp.listen(8)
        self._tcp.settimeout(0.5)
        self._running = True

        LOGGER.info(
            "控制通道监听 %s:%d，对外通告 %s，UDP 端口池 %d-%d",
            self.cfg.bind_ip,
            self.cfg.control_port,
            self.cfg.advertise_ip,
            self.cfg.udp_port_start,
            self.cfg.udp_port_end,
        )

        while self._running:
            try:
                conn, addr = self._tcp.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(
                target=self._handle_conn, args=(conn, addr), daemon=True
            ).start()

    def _handle_conn(self, conn: socket.socket, addr: tuple[str, int]) -> None:
        LOGGER.debug("控制连接来自 %s:%d", *addr)
        conn.settimeout(self.cfg.control_idle_frame_ms / 1000.0)
        buffer = b""
        try:
            while self._running:
                try:
                    chunk = conn.recv(2048)
                except socket.timeout:
                    if buffer.strip():
                        self._dispatch(buffer, conn)
                        buffer = b""
                    continue
                except OSError:
                    break

                if not chunk:
                    if buffer.strip():
                        self._dispatch(buffer, conn)
                    break

                buffer += chunk
                buffer = self._drain(buffer, conn)
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _drain(self, buffer: bytes, conn: socket.socket) -> bytes:
        """按 NUL / CRLF 切帧，返回剩余未处理数据。"""
        while True:
            cut = -1
            cut_len = 0
            for delim in (b"\x00", b"\r\n", b"\n"):
                idx = buffer.find(delim)
                if idx != -1 and (cut == -1 or idx < cut):
                    cut = idx
                    cut_len = len(delim)
            if cut == -1:
                return buffer
            frame, buffer = buffer[:cut], buffer[cut + cut_len:]
            if frame.strip():
                self._dispatch(frame, conn)

    def _dispatch(self, frame: bytes, conn: socket.socket) -> None:
        text = frame.decode("utf-8", "ignore").strip().strip("\x00")
        if "cmd=" not in text:
            LOGGER.debug("忽略非指令数据: %r", text[:80])
            return

        params = parse_kv(text)
        cmd = params.get("cmd", "")
        LOGGER.debug("收到指令 %s", text[:200])

        if cmd == "RecordStartRequest":
            ok, reason = self.handle_start(params)
            session_id = params.get("id", "").strip()
            if ok:
                with self._lock:
                    session = self._sessions.get(session_id)
                port = session.port if session else 0
                reply = (
                    f"cmd=RecordStartResponse&id={session_id}"
                    f"&result=success&reason=&server={self.cfg.advertise_ip}&port={port}"
                )
            else:
                reply = (
                    f"cmd=RecordStartResponse&id={session_id}"
                    f"&result=failed&reason={reason}&server=&port=0"
                )
            self._send(conn, reply)

        elif cmd == "RecordStopRequest":
            self.handle_stop(params)
            self._send(
                conn,
                f"cmd=RecordStopResponse&id={params.get('id', '')}&result=success",
            )
        else:
            LOGGER.warning("未知指令: %s", cmd)

    def _send(self, conn: socket.socket, text: str) -> None:
        try:
            conn.sendall((text + self.cfg.control_terminator).encode("utf-8"))
            LOGGER.debug("回复 %s", text)
        except OSError:
            LOGGER.exception("回复失败")

    # -- 关停 -------------------------------------------------------------- #

    def shutdown(self) -> None:
        self._running = False
        if self._tcp is not None:
            try:
                self._tcp.close()
            except OSError:
                pass
            self._tcp = None

        with self._lock:
            sessions = list(self._sessions.values())
        for session in sessions:
            session.stop("服务退出")
        for session in sessions:
            session.join(3.0)


# --------------------------------------------------------------------------- #
# 入口
# --------------------------------------------------------------------------- #


def setup_logging(cfg: Config) -> None:
    Path(cfg.log_dir).mkdir(parents=True, exist_ok=True)
    log_file = Path(cfg.log_dir) / "server.log"

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
    """Start the optional receiver only after the operator supplies its reachable IPv4."""
    parser = argparse.ArgumentParser(description="Fanvil 通话录音服务器")
    parser.add_argument("--config", help="配置文件路径")
    parser.add_argument("--port", type=int, help="控制端口（默认 10000）")
    parser.add_argument("--ip", help="对外通告的本机 IP")
    parser.add_argument("--dir", help="录音输出目录")
    parser.add_argument("--verbose", action="store_true", help="输出调试日志")
    args = parser.parse_args()

    cfg = Config.load(args.config)
    if args.port:
        cfg.control_port = args.port
    if args.ip:
        cfg.advertise_ip = args.ip
    if args.dir:
        cfg.record_dir = args.dir

    if not cfg.advertise_ip:
        parser.error("Set advertise_ip in config.json or pass --ip before starting")

    setup_logging(cfg)
    if args.verbose:
        logging.getLogger().handlers[0].setLevel(logging.DEBUG)

    recorder = Recorder(cfg)
    try:
        recorder.serve_forever()
    except KeyboardInterrupt:
        LOGGER.info("收到中断，正在退出")
    finally:
        recorder.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
