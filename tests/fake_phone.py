"""模拟 Fanvil 话机，用于在不动真机的情况下验证录音服务。

它按官方协议扮演话机侧：
    1. TCP 连控制端口，发 RecordStartRequest
    2. 解析 RecordStartResponse 拿到 server / port
    3. 向该 UDP 端口推合成 RTP（默认 1kHz 正弦，PCMA）
    4. 发 RecordStopRequest，收到 RecordStopResponse

用法：
    python tests/fake_phone.py
    python tests/fake_phone.py --duration 5 --codec PCMU --local 301 --remote 302
"""

from __future__ import annotations

import argparse
import math
import random
import socket
import struct
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import g711

SAMPLE_RATE = 8000
FRAME_MS = 20
SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS // 1000


def build_tone(frames: int, codec: str, amplitude: float = 0.45) -> list[bytes]:
    """生成 frames 帧 G.711 载荷，频率在 1kHz 与 600Hz 间交替，便于人耳分辨。"""
    payloads: list[bytes] = []
    phase = 0.0
    for index in range(frames):
        freq = 1000.0 if (index // 25) % 2 == 0 else 600.0
        pcm = bytearray()
        for _ in range(SAMPLES_PER_FRAME):
            value = int(amplitude * 32767 * math.sin(phase))
            pcm += struct.pack("<h", value)
            phase += 2 * math.pi * freq / SAMPLE_RATE
        payloads.append(g711.encode(bytes(pcm), codec))
    return payloads


def make_rtp(seq: int, timestamp: int, ssrc: int, payload: bytes, codec: str) -> bytes:
    header = struct.pack(
        ">BBHII",
        0x80,
        g711.codec_to_pt(codec),
        seq & 0xFFFF,
        timestamp & 0xFFFFFFFF,
        ssrc,
    )
    return header + payload


def read_line(sock: socket.socket, timeout: float = 5.0) -> str:
    sock.settimeout(timeout)
    buffer = b""
    while b"\r\n" not in buffer and b"\x00" not in buffer:
        chunk = sock.recv(1024)
        if not chunk:
            break
        buffer += chunk
    return buffer.decode("utf-8", "ignore").strip().strip("\x00")


def parse_kv(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for part in text.split("&"):
        if "=" in part:
            key, value = part.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="模拟 Fanvil 话机录音")
    parser.add_argument("--host", default="127.0.0.1", help="录音服务器地址")
    parser.add_argument("--port", type=int, default=10000, help="控制端口")
    parser.add_argument("--duration", type=float, default=3.0, help="录音时长（秒）")
    parser.add_argument("--codec", default="PCMA", choices=["PCMA", "PCMU"])
    parser.add_argument("--local", default="301", help="本端号码")
    parser.add_argument("--remote", default="302", help="对端号码")
    parser.add_argument("--remote-name", default="", help="对端姓名（留空表示话机未解析出姓名）")
    parser.add_argument("--dir", default="out", choices=["in", "out"], help="通话方向")
    parser.add_argument("--no-terminator", action="store_true", help="指令不带终止符")
    parser.add_argument("--no-stop", action="store_true", help="不发 Stop，模拟话机异常中断")
    parser.add_argument("--stop-timeout", type=float, default=8.0, help="等待 WAV 落盘的秒数")
    args = parser.parse_args()

    terminator = "" if args.no_terminator else "\r\n"
    session_id = str(random.randint(100000, 999999))
    ssrc = random.randint(1, 0xFFFFFFFF)

    start_req = (
        f"cmd=RecordStartRequest&id={session_id}"
        f"&device=00:a8:59:ec:27:1f"
        f"&remote_number={args.remote}&remote_name={args.remote_name}"
        f"&local_number={args.local}&local_name=local"
        f"&dir={args.dir}&codec={args.codec}"
    )

    print(f"[1] 连接 {args.host}:{args.port}")
    with socket.create_connection((args.host, args.port), timeout=5) as control:
        print(f"[2] 发送 RecordStartRequest id={session_id}")
        control.sendall((start_req + terminator).encode())

        reply = read_line(control)
        print(f"    响应: {reply}")
        params = parse_kv(reply)
        if params.get("result") != "success":
            print("!! 启动失败:", reply)
            return 1

        udp_host = params.get("server") or args.host
        udp_port = int(params.get("port") or 0)
        if not udp_port:
            print("!! 响应里没有 UDP 端口")
            return 1
        print(f"[3] 向 {udp_host}:{udp_port} 推送 RTP（{args.codec}，{args.duration}s）")

        frames = max(1, int(args.duration * 1000 / FRAME_MS))
        payloads = build_tone(frames, args.codec)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as media:
            timestamp = random.randint(0, 0xFFFF)
            seq = random.randint(0, 0xFFFF)
            interval = FRAME_MS / 1000.0
            next_at = time.perf_counter()
            for payload in payloads:
                media.sendto(
                    make_rtp(seq, timestamp, ssrc, payload, args.codec),
                    (udp_host, udp_port),
                )
                seq += 1
                timestamp += SAMPLES_PER_FRAME
                next_at += interval
                delay = next_at - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)

        print(f"[4] 发送 RecordStopRequest（共 {frames} 包）")
        if args.no_stop:
            print("    跳过 Stop，直接断开控制连接（模拟话机异常）")
        else:
            control.sendall(
                (f"cmd=RecordStopRequest&id={session_id}&reason=normal{terminator}").encode()
            )
            print(f"    响应: {read_line(control)}")

    expected = int(args.duration * SAMPLE_RATE)
    deadline = time.time() + args.stop_timeout
    while time.time() < deadline:
        candidates = sorted(
            (Path("recordings")).glob(f"*_{args.local}_{args.remote}_{args.dir}*.wav"),
            key=lambda p: p.stat().st_mtime,
        )
        if candidates:
            latest = candidates[-1]
            with wave.open(str(latest), "rb") as handle:
                frames_written = handle.getnframes()
                rate = handle.getframerate()
                width = handle.getsampwidth()
                channels = handle.getnchannels()
            if frames_written > 0:
                print(f"[5] 落盘验证: {latest}")
                print(
                    f"    采样率={rate} 位深={width * 8}bit 声道={channels} "
                    f"帧数={frames_written} 时长={frames_written / rate:.2f}s"
                )
                tolerance = SAMPLE_RATE * 0.25
                if abs(frames_written - expected) > tolerance:
                    print(f"!! 时长偏差过大，期望约 {expected} 帧")
                    return 1
                print("[6] 通过")
                return 0
        time.sleep(0.3)

    print("!! 超时：没有找到已落盘的 WAV")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
