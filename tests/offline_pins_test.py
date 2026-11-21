"""Check compiled speech pins against the installer sources. No model bytes are downloaded."""

from pathlib import Path
import json
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def pins():
    """Read the pin table that the runtime and packager both trust."""
    text = (ROOT / "cpp" / "offline_rules.hpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'\{"(sense|qwen|qwen17)", "([^"]+)", (\d+)ULL, "([0-9a-f]{64})"\}',
        text,
    )
    if len(rows) < 17:
        raise SystemExit(f"expected the full pin table, found {len(rows)}")
    return rows


def main():
    """Fail when a published hash or length drifts from the download scripts."""
    found = pins()
    sense = {path: (int(size), digest) for model, path, size, digest in found if model == "sense"}
    installer = (ROOT / "cpp" / "speech" / "install.ps1").read_text(encoding="utf-8")
    expected = {
        "models/model.int8.onnx": "model.int8.onnx",
        "models/tokens.txt": "tokens.txt",
        "models/silero_vad.onnx": "silero_vad.onnx",
    }
    for relative, leaf in expected.items():
        size, digest = sense[relative]
        if f"'{digest}'" not in installer or str(size) not in installer or leaf not in installer:
            raise SystemExit(f"install.ps1 does not pin {relative}")
    assets = json.loads((ROOT / "cpp" / "speech" / "qwen-assets.json").read_text(encoding="utf-8"))
    catalog = {
        "qwen": {item["file"]: item for item in assets["models"]},
        "qwen17": {item["file"]: item for item in assets["models17"]},
    }
    prefixes = {"qwen": "qwen/models/", "qwen17": "qwen/models-1.7b/"}
    for model, path, size, digest in found:
        if model == "sense":
            continue
        leaf = path[len(prefixes[model]):]
        item = catalog[model][leaf]
        if int(item["size"]) != int(size) or item["sha256"] != digest:
            raise SystemExit(f"qwen-assets.json drifted for {path}")
    package = (ROOT / "tools" / "New-OfflinePackage.ps1").read_text(encoding="utf-8")
    for needle in ("PhoneRecord-x64-offline-sensevoice.zip", "PhoneRecord-x64-offline-qwen06.zip", "kGitHubReleaseAssetMaxBytes", "offline_rules.hpp"):
        if needle not in package:
            raise SystemExit(f"packager is missing {needle}")
    print("OFFLINE_PINS_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
