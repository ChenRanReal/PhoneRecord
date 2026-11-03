"""通话记录聚合层。

两个数据源：

    recordings/calls.csv   已录音的通话，本地永久保存，时间与时长精确
    /calllog.xml           话机的完整通话记录（600 条上限），含未接、未录音

以 calls.csv 为骨架，用 calllog 补充那些「没有录音」的通话（未接来电等），
按「号码相同 + 时间接近」去重，最终按时间倒序输出。
"""

from __future__ import annotations

import csv
import logging
import re
import threading
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

LOGGER = logging.getLogger("callstore")

# 话机 calllog 的 type 字段
TYPE_INCOMING = "0"
TYPE_OUTGOING = "1"
TYPE_MISSED = "2"

MATCH_WINDOW = 180  # 号码对得上时，时间差多少秒内认为是同一通


@dataclass
class CallRecord:
    time: datetime
    direction: str          # in / out / missed
    number: str
    name: str
    duration: int           # 秒
    file: str | None = None # 录音文件名（相对 recordings/）
    size: int = 0
    line: str = ""

    @property
    def has_audio(self) -> bool:
        return bool(self.file)

    def to_dict(self) -> dict:
        return {
            "time": self.time.strftime("%Y-%m-%d %H:%M:%S"),
            "date": self.time.strftime("%Y-%m-%d"),
            "clock": self.time.strftime("%H:%M"),
            "direction": self.direction,
            "number": self.number,
            "name": self.name,
            "duration": self.duration,
            "duration_text": self._duration_text(),
            "file": self.file,
            "size": self.size,
            "line": self.line,
            "has_audio": self.has_audio,
        }

    def _duration_text(self) -> str:
        if self.direction == "missed":
            return "未接"
        if self.duration <= 0:
            return "-"
        minutes, seconds = divmod(self.duration, 60)
        if minutes >= 60:
            hours, minutes = divmod(minutes, 60)
            return f"{hours}:{minutes:02d}:{seconds:02d}"
        return f"{minutes}:{seconds:02d}"


def _text(block: str, tag: str) -> str:
    match = re.search(r"<%s>\s*(.*?)\s*</%s>" % (tag, tag), block, re.S)
    return match.group(1).strip() if match else ""


def parse_calllog(xml: str) -> list[CallRecord]:
    """解析话机通话记录。time 字段是「本地时间当 UTC 存」的 epoch。"""
    records: list[CallRecord] = []
    for block in re.findall(r"<record>(.*?)</record>", xml, re.S):
        try:
            stamp = int(_text(block, "time") or 0)
            when = datetime.utcfromtimestamp(stamp)
        except (ValueError, OSError):
            continue

        raw_type = _text(block, "type")
        if raw_type == TYPE_MISSED:
            direction = "missed"
        elif raw_type == TYPE_OUTGOING:
            direction = "out"
        else:
            direction = "in"

        try:
            duration = int(_text(block, "dur") or 0)
        except ValueError:
            duration = 0

        records.append(
            CallRecord(
                time=when,
                direction=direction,
                number=_text(block, "num"),
                name=_text(block, "name"),
                duration=duration,
                line=_text(block, "line"),
            )
        )
    return records


def parse_ledger(path: Path, record_dir: Path) -> list[CallRecord]:
    """读取 calls.csv，只保留文件仍然存在的行。"""
    if not path.exists():
        return []

    records: list[CallRecord] = []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                when = datetime.strptime(row["开始时间"], "%Y-%m-%d %H:%M:%S")
                duration = int(float(row.get("时长秒") or 0))
            except (KeyError, ValueError):
                continue

            filename = (row.get("文件名") or "").strip()
            audio = record_dir / filename if filename else None
            if audio is not None and not audio.exists():
                audio = None

            records.append(
                CallRecord(
                    time=when,
                    direction=(row.get("方向") or "").strip() or "in",
                    number=(row.get("对端号码") or "").strip(),
                    name=(row.get("对端姓名") or "").strip(),
                    duration=duration,
                    file=filename if audio else None,
                    size=audio.stat().st_size if audio else 0,
                )
            )
    return records


def parse_filename(path: Path) -> CallRecord | None:
    """从录音文件名反推记录，用于 calls.csv 缺失时重建。

    命名规则：<日期>-<时间>_<本端>_<对端>_<in|out>[_<姓名>].wav
    """
    stem = path.stem
    parts = stem.split("_")
    if len(parts) < 4:
        return None

    try:
        when = datetime.strptime(parts[0], "%Y%m%d-%H%M%S")
    except ValueError:
        return None

    direction = parts[3] if parts[3] in ("in", "out") else "in"
    name = "_".join(parts[4:]) if len(parts) > 4 else ""

    duration = 0
    try:
        import wave

        with wave.open(str(path), "rb") as handle:
            duration = int(handle.getnframes() / max(1, handle.getframerate()))
    except Exception:  # noqa: BLE001
        duration = 0

    try:
        size = path.stat().st_size
    except OSError:
        size = 0

    return CallRecord(
        time=when,
        direction=direction,
        number=parts[2],
        name=name,
        duration=duration,
        file=path.name,
        size=size,
    )


def scan_recordings(record_dir: Path) -> list[CallRecord]:
    if not record_dir.exists():
        return []
    result: list[CallRecord] = []
    for path in sorted(record_dir.glob("*.wav")):
        record = parse_filename(path)
        if record is not None:
            result.append(record)
    return result


class CallStore:
    """线程安全的通话记录视图。"""

    def __init__(
        self,
        record_dir: Path,
        ledger: Path,
        directory_file: Path,
        extra_file: Path | None = None,
    ) -> None:
        self.record_dir = record_dir
        self.ledger = ledger
        self.directory_file = directory_file
        self.extra_file = extra_file
        self._records: list[CallRecord] = []
        self._lock = threading.Lock()
        self._last_refresh: datetime | None = None
        self._calllog: list[CallRecord] = []
        self._names: dict[str, str] = {}

    # -- 名称 -------------------------------------------------------------- #

    def load_names(self) -> dict[str, str]:
        """directory.json 是同步产物，contacts_extra.json 是手工补充，后者优先。"""
        import json

        names: dict[str, str] = {}
        try:
            data = json.loads(self.directory_file.read_text(encoding="utf-8"))
            names.update(data.get("names", {}))
        except (OSError, ValueError):
            pass

        if self.extra_file is not None:
            try:
                extra = json.loads(self.extra_file.read_text(encoding="utf-8"))
                if isinstance(extra, dict):
                    names.update({str(k): str(v) for k, v in extra.items()})
            except (OSError, ValueError):
                pass

        self._names = names
        return self._names

    def resolve_name(self, number: str, fallback: str = "") -> str:
        return fallback or self._names.get(number, "")

    # -- 刷新 -------------------------------------------------------------- #

    def update_calllog(self, records: list[CallRecord]) -> None:
        with self._lock:
            self._calllog = records
        self.rebuild()

    def rebuild(self) -> None:
        self.load_names()
        recorded = parse_ledger(self.ledger, self.record_dir)

        # calls.csv 缺失或不完整时，用文件名兜底，避免录音在界面上「消失」
        known_files = {item.file for item in recorded if item.file}
        for item in scan_recordings(self.record_dir):
            if item.file not in known_files:
                recorded.append(item)

        with self._lock:
            calllog = list(self._calllog)

        merged: list[CallRecord] = list(recorded)
        used = [False] * len(recorded)

        for entry in calllog:
            hit = None
            for index, candidate in enumerate(recorded):
                if used[index]:
                    continue
                if candidate.number != entry.number:
                    continue
                if abs((candidate.time - entry.time).total_seconds()) <= MATCH_WINDOW:
                    hit = index
                    break

            if hit is not None:
                used[hit] = True
                # 录音记录本身更精确，只把话机解析出的姓名补上
                if not merged[hit].name and entry.name:
                    merged[hit].name = entry.name
                continue

            entry.name = self.resolve_name(entry.number, entry.name)
            merged.append(entry)

        for index, record in enumerate(merged):
            if not record.name:
                record.name = self.resolve_name(record.number, record.name)

        merged.sort(key=lambda item: item.time, reverse=True)

        with self._lock:
            self._records = merged
            self._last_refresh = datetime.now()

    # -- 查询 -------------------------------------------------------------- #

    def query(
        self,
        direction: str = "all",
        keyword: str = "",
        limit: int = 300,
    ) -> list[CallRecord]:
        with self._lock:
            records = list(self._records)

        if direction in ("in", "out", "missed"):
            records = [item for item in records if item.direction == direction]
        elif direction == "recorded":
            records = [item for item in records if item.has_audio]

        if keyword:
            needle = keyword.strip().lower()
            records = [
                item
                for item in records
                if needle in item.number.lower() or needle in item.name.lower()
            ]

        return records[:limit]

    def stats(self) -> dict:
        with self._lock:
            records = list(self._records)
        today = datetime.now().date()
        today_items = [item for item in records if item.time.date() == today]
        return {
            "total": len(records),
            "recorded": sum(1 for item in records if item.has_audio),
            "today": len(today_items),
            "today_recorded": sum(1 for item in today_items if item.has_audio),
            "last_refresh": self._last_refresh.strftime("%H:%M:%S") if self._last_refresh else "-",
        }

    def unknown_numbers(self) -> list[dict]:
        """出现次数最多的未知号码，用于快速补通讯录。"""
        with self._lock:
            records = list(self._records)
        counter: dict[str, int] = {}
        for item in records:
            if item.number and not item.name:
                counter[item.number] = counter.get(item.number, 0) + 1
        return [
            {"number": number, "count": count}
            for number, count in sorted(counter.items(), key=lambda kv: -kv[1])
        ]
