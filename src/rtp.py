"""最小 RTP 解析（RFC 3550）。

只解出录音需要的字段：版本、padding、扩展、CSRC、marker、payload type、
序列号、时间戳、SSRC 与载荷。字段名与 RFC 保持一致，便于对照抓包。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

RTP_MIN_HEADER = 12
RTP_VERSION = 2


@dataclass(frozen=True)
class RtpPacket:
    version: int
    padding: bool
    extension: bool
    csrc_count: int
    marker: bool
    payload_type: int
    sequence: int
    timestamp: int
    ssrc: int
    payload: bytes


def parse(packet: bytes) -> RtpPacket | None:
    """解析 RTP 包；非法或不完整时返回 None。"""
    if len(packet) < RTP_MIN_HEADER:
        return None

    b0 = packet[0]
    version = b0 >> 6
    if version != RTP_VERSION:
        return None

    padding = bool((b0 >> 5) & 1)
    extension = bool((b0 >> 4) & 1)
    csrc_count = b0 & 0x0F

    b1 = packet[1]
    marker = bool((b1 >> 7) & 1)
    payload_type = b1 & 0x7F

    sequence, timestamp, ssrc = struct.unpack(">HII", packet[2:12])

    offset = RTP_MIN_HEADER + csrc_count * 4
    if len(packet) < offset:
        return None

    if extension:
        if len(packet) < offset + 4:
            return None
        ext_words = struct.unpack(">H", packet[offset + 2:offset + 4])[0]
        offset += 4 + ext_words * 4
        if len(packet) < offset:
            return None

    payload = packet[offset:]
    if padding and payload:
        pad_len = payload[-1]
        if 0 < pad_len <= len(payload):
            payload = payload[:-pad_len]

    return RtpPacket(
        version=version,
        padding=padding,
        extension=extension,
        csrc_count=csrc_count,
        marker=marker,
        payload_type=payload_type,
        sequence=sequence,
        timestamp=timestamp,
        ssrc=ssrc,
        payload=payload,
    )
