"""G.711 A-law / mu-law 解码。

Python 3.13 移除了 audioop，这里用标准 G.711 算法预生成 256 项查找表，
解码时按字节查表 -> 16bit 线性 PCM，速度与 audioop 同量级。
"""

from __future__ import annotations

import array
import sys


def _build_alaw_table() -> list[int]:
    """ITU-T G.711 A-law -> 14bit 线性码。"""
    table: list[int] = []
    for i in range(256):
        a = i ^ 0x55
        t = (a & 0x0F) << 4
        seg = (a & 0x70) >> 4
        if seg == 0:
            t += 8
        elif seg == 1:
            t += 0x108
        else:
            t = (t + 0x108) << (seg - 1)
        table.append(t if (a & 0x80) else -t)
    return table


def _build_ulaw_table() -> list[int]:
    """ITU-T G.711 mu-law -> 14bit 线性码。"""
    table: list[int] = []
    for i in range(256):
        u = ~i & 0xFF
        t = ((u & 0x0F) << 3) + 0x84
        t <<= (u & 0x70) >> 4
        table.append((0x84 - t) if (u & 0x80) else (t - 0x84))
    return table


ALAW_TABLE = _build_alaw_table()
ULAW_TABLE = _build_ulaw_table()


def decode(data: bytes, codec: str) -> bytes:
    """把 G.711 载荷解码为小端 16bit 单声道 PCM。"""
    key = codec.upper()
    if key in ("PCMA", "G711A", "G711_A", "ALAW"):
        table = ALAW_TABLE
    elif key in ("PCMU", "G711U", "G711_U", "ULAW"):
        table = ULAW_TABLE
    else:
        raise ValueError(f"不支持的编码: {codec}")

    pcm = array.array("h", (table[b] for b in data))
    if sys.byteorder == "big":
        pcm.byteswap()
    return pcm.tobytes()


def codec_to_pt(codec: str) -> int:
    """把编码名映射为 RTP payload type（静态表）。"""
    key = codec.upper()
    if key.startswith("PCMU"):
        return 0
    return 8


def pt_to_codec(pt: int) -> str:
    """RTP payload type 反查编码名。"""
    if pt == 0:
        return "PCMU"
    if pt == 8:
        return "PCMA"
    return f"PT{pt}"


# --------------------------------------------------------------------------- #
# 编码方向（自测与离线转换用）
# --------------------------------------------------------------------------- #

_SEG_AEND = (0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF)
_SEG_UEND = (0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF)
_QUANT_MASK = 0xF
_SEG_SHIFT = 4


def _search(value: int, table: tuple[int, ...]) -> int:
    for index, bound in enumerate(table):
        if value <= bound:
            return index
    return len(table)


def _linear_to_alaw(sample: int) -> int:
    sample >>= 3
    if sample >= 0:
        mask = 0xD5
    else:
        mask = 0x55
        sample = -sample - 1

    seg = _search(sample, _SEG_AEND)
    if seg >= 8:
        return 0x7F ^ mask

    aval = seg << _SEG_SHIFT
    if seg < 2:
        aval |= (sample >> 1) & _QUANT_MASK
    else:
        aval |= (sample >> seg) & _QUANT_MASK
    return aval ^ mask


def _linear_to_ulaw(sample: int) -> int:
    mask = 0xFF if sample >= 0 else 0x7F
    if sample < 0:
        sample = -sample
    sample = min(sample, 32635) + 0x84

    seg = _search(sample, _SEG_UEND)
    if seg >= 8:
        return 0x7F ^ mask

    uval = (seg << _SEG_SHIFT) | ((sample >> (seg + 3)) & _QUANT_MASK)
    return uval ^ mask


def encode(pcm: bytes, codec: str) -> bytes:
    """把 16bit 线性 PCM 编码为 G.711，主要用于生成测试流。"""
    pcm_view = array.array("h")
    pcm_view.frombytes(pcm)
    if sys.byteorder == "big":
        pcm_view.byteswap()

    key = codec.upper()
    if key in ("PCMA", "G711A", "G711_A", "ALAW"):
        func = _linear_to_alaw
    elif key in ("PCMU", "G711U", "G711_U", "ULAW"):
        func = _linear_to_ulaw
    else:
        raise ValueError(f"不支持的编码: {codec}")

    return bytes(func(sample) for sample in pcm_view)
