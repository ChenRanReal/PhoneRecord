"""Exercise the real two-mode settings, locale switching and persistence in owned offline processes."""

from __future__ import annotations

import argparse
import ctypes
import json
import pathlib
import tempfile

from desktop_smoke import USER, dismiss_dialog, set_text, title
from desktop_workflow import main_window
from localization_smoke import capture_page, resources, select_help, select_language
from product_smoke import App


def select_mode(window, index: int):
    """Use the same dropdown selection notification as the native user action."""
    control = USER.GetDlgItem(window, 416)
    USER.SendMessageW(control, 0x014E, index, 0)
    USER.SendMessageW(window, 0x0111, (1 << 16) | 416, control)


def main() -> int:
    """Verify real controls and saved files while leaving the user's settings and phone untouched."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    USER.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    USER.IsWindowVisible.argtypes = [ctypes.c_void_p]
    checks = [f"three-language resources: {resources()}"]
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-address-ui-") as folder:
        root = pathlib.Path(folder)
        for mode, token in enumerate(("fixed", "dhcp")):
            app = App(root, args.exe.resolve(), output, token, gui=True)
            app.args.extend(["--capture-dir", str(output)])
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 103, 0)
                USER.SendMessageW(window, 0x0111, 433, 0)
                assert USER.SendMessageW(USER.GetDlgItem(window, 416), 0x0147, 0, 0) == 0
                select_mode(window, mode)
                if mode == 1:
                    USER.PostMessageW(window, 0x0111, 420, 0)
                    dismiss_dialog(app.process.pid, "phone_mac")
                set_text(USER.GetDlgItem(window, 417), "02-AA-BB-CC-DD-01")
                for language, expected in enumerate(("话机寻址方式", "話機尋址方式", "Phone addressing")):
                    select_language(window, language)
                    USER.SendMessageW(window, 0x0111, 103, 0)
                    assert title(USER.GetDlgItem(window, 726)) == expected
                    assert USER.SendMessageW(USER.GetDlgItem(window, 416), 0x0147, 0, 0) == mode
                    assert bool(USER.IsWindowEnabled(USER.GetDlgItem(window, 400))) == True
                    assert bool(USER.IsWindowEnabled(USER.GetDlgItem(window, 417))) == (mode == 1)
                    capture_page(window, output, f"{token}-{language}", 3)
                    help_text = select_help(window, 1)
                    assert "phone_address_mode" in help_text and "phone_mac" in help_text and "phone_host" not in help_text
                USER.SendMessageW(window, 0x0111, 103, 0)
                USER.PostMessageW(window, 0x0111, 420, 0)
                dismiss_dialog(app.process.pid)
                saved = app.config.read_text(encoding="utf-8")
                assert f"phone_address_mode={token}\n" in saved
                assert "phone_mac=02-AA-BB-CC-DD-01\n" in saved
                assert "PHONE_ADDRESS_CHANGED" not in app.log(), "offline settings must not start discovery"
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 103, 0)
                assert USER.SendMessageW(USER.GetDlgItem(window, 416), 0x0147, 0, 0) == mode
                assert title(USER.GetDlgItem(window, 417)) == "02-AA-BB-CC-DD-01"
            checks.append(f"{token}: validation, locale selection, visibility, save and restart")
    (output / "address-ui-results.json").write_text(json.dumps({"passed": checks, "real_phone_contacted": False}, indent=2) + "\n", encoding="utf-8")
    print(f"PHONE_ADDRESS_UI_OK modes=2 languages=3 checks={len(checks)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
