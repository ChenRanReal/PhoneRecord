"""同步「号码 -> 姓名」目录。

数据来源是话机自己已经解析好的两份数据，不需要碰 LDAP（密码在话机上是隐藏的）：

    /contacts.xml   本地电话本（用户手工维护，权威）
    /calllog.xml    通话记录，name 字段是话机通过 LDAP 云通讯录解析出来的

合并后写入 directory.json，供录音服务在起录瞬间查名字使用。

用法：
    python tools/sync_directory.py
"""

from __future__ import annotations

import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from phone_client import PhoneClient

BASE = Path(__file__).resolve().parent.parent
CACHE = BASE / "directory.json"


def _text(block: str, tag: str) -> str:
    match = re.search(r"<%s>\s*(.*?)\s*</%s>" % (tag, tag), block, re.S)
    return match.group(1).strip() if match else ""


def normalize(number: str) -> str:
    """只保留数字和开头的 +，去掉空格、横杠等。"""
    number = (number or "").strip()
    if not number:
        return ""
    cleaned = re.sub(r"[^\d+]", "", number)
    return cleaned


def parse_contacts(xml: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for block in re.findall(r"<contact>(.*?)</contact>", xml, re.S):
        name = _text(block, "name")
        if not name:
            continue
        for tag in ("tel1", "tel2", "tel3"):
            number = normalize(_text(block, tag))
            if number:
                result[number] = name
    return result


def parse_calllog(xml: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for block in re.findall(r"<record>(.*?)</record>", xml, re.S):
        name = _text(block, "name")
        number = normalize(_text(block, "num"))
        if name and number:
            result[number] = name
    return result


def sync(host: str | None = None) -> int:
    """Synchronize the operator-selected phone using explicit or environment credentials."""
    client = PhoneClient(host=host)
    if not client.login():
        print("!! 登录话机失败")
        return 1

    stamp = int(time.time() * 1000)

    _, contacts_xml = client.get(f"/contacts.xml?now={stamp}")
    local = parse_contacts(contacts_xml)

    _, calllog_xml = client.get(f"/calllog.xml?now={stamp}")
    history = parse_calllog(calllog_xml)

    # 本地电话本是手工维护的，优先；通话记录补充它没有的号码
    merged: dict[str, str] = dict(history)
    merged.update(local)

    payload = {
        "updated_at": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "source": {"local_contacts": len(local), "from_calllog": len(history)},
        "names": dict(sorted(merged.items())),
    }
    CACHE.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    print(f"本地电话本 : {len(local)} 个号码")
    print(f"通话记录   : {len(history)} 个带姓名的号码")
    print(f"合并结果   : {len(merged)} 个号码 -> {CACHE}")
    sample = list(merged.items())[:5]
    for number, name in sample:
        print(f"   {number}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(sync())
