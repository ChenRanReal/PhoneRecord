"""Verify secondary action menus on an owned hidden, offline native process."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import tempfile
from ctypes import wintypes as w

from product_smoke import App, wait_for
from desktop_smoke import USER, windows, title, set_text
from desktop_workflow import main_window
from compact_ui_smoke import shown, group, bounds_fit
from localization_smoke import select_language, select_help, capture_page, resources


def menu_items(app, window, anchor):
    """Inspect and cancel only the popup belonging to this test's child process."""
    USER.GetMenuItemCount.argtypes = [w.HMENU]
    USER.GetMenuItemID.argtypes = [w.HMENU, ctypes.c_int]
    USER.GetMenuState.argtypes = [w.HMENU, w.UINT, w.UINT]
    USER.PostMessageW(window, 0x0111, anchor, 0)
    popup = wait_for(lambda: next(iter(windows(app.process.pid, '#32768')), None))
    try:
        menu = wait_for(lambda: USER.SendMessageW(popup, 0x01E1, 0, 0))  # MN_GETHMENU.
        assert menu
        return {USER.GetMenuItemID(menu, i): USER.GetMenuState(menu, i, 0x400)
                for i in range(USER.GetMenuItemCount(menu))}
    finally:
        USER.ShowWindow(popup, 0)
        USER.PostMessageW(window, 0x001F, 0, 0)
        wait_for(lambda: not windows(app.process.pid, '#32768'))


def main():
    """Check menu reachability, conditional controls, draft isolation and all locales."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-action-menu-') as temporary:
        app = App(Path(temporary), exe, output, 'menus', gui=True)
        app.args += ['--capture-dir', str(output)]
        original = app.config.read_bytes()
        with app:
            window = main_window(app)
            for language in range(3):
                select_language(window, language)
                USER.SendMessageW(window, 0x0111, 100, 0)
                assert shown(window, 502) and shown(window, 506)
                assert not USER.GetDlgItem(window, 504)
                assert set(menu_items(app, window, 506)) == {504, 422}
                capture_page(window, output, f'overview_{language}', 0)

                USER.SendMessageW(window, 0x0111, 102, 0)
                assert shown(window, 312) and shown(window, 315)
                assert not USER.GetDlgItem(window, 313) and not USER.GetDlgItem(window, 314)
                assert set(menu_items(app, window, 315)) == {313, 314}
                capture_page(window, output, f'diagnostics_{language}', 2)

                for category in range(3):
                    group(window, category)
                    assert shown(window, 420) and shown(window, 435)
                    assert shown(window, 424) == (category == 0)
                    assert not USER.GetDlgItem(window, 422) and not USER.GetDlgItem(window, 423)
                    assert set(menu_items(app, window, 435)) == {423, 504, 422}
                    bounds_fit(window)
                    capture_page(window, output, f'settings_{language}_{category}', 3)

                for topic in range(7):
                    select_help(window, topic)
                    assert shown(window, 804) == (topic == 5)
                    assert shown(window, 805) == (topic == 5)
                select_help(window, 0)
                capture_page(window, output, f'help_{language}', 4)
                select_help(window, 5)
                capture_page(window, output, f'report_{language}', 4)

                USER.SendMessageW(window, 0x0111, 105, 0)
                assert shown(window, 820) and shown(window, 830)
                assert all(not shown(window, i) for i in (821, 822, 824, 825, 828))
                assert {840, 841, 842, 843, 844, 845} <= set(menu_items(app, window, 830))
                capture_page(window, output, f'updates_{language}', 5)
            checks.append('all three locales retain accessible More menus; secondary actions are removed from page toolbars')
            checks.append('Configure phone is shown only in Basic; Help report actions appear only on the inspection topic')
            checks.append('no-update state shows Check updates and preferences without unavailable download/install buttons')

            group(window, 0)
            set_text(USER.GetDlgItem(window, 400), '192.0.2.77')
            USER.SendMessageW(window, 0x0111, 422, 0)
            assert USER.SendMessageW(USER.GetDlgItem(window, 800), 0x0147, 0, 0) == 6
            assert title(USER.GetDlgItem(window, 400)) == '192.0.2.77'
            assert app.config.read_bytes() == original
            USER.SendMessageW(window, 0x0111, 105, 0)
            USER.SendMessageW(window, 0x0111, 844, 0)
            USER.SendMessageW(window, 0x0111, 845, 0)
            items = menu_items(app, window, 830)
            assert items[844] & 0x8 and not items[842] & 0x8
            assert (app.config.with_name(app.config.name + '.updates')).exists()
            assert app.config.read_bytes() == original
            assert title(USER.GetDlgItem(window, 400)) == '192.0.2.77'
            checks.append('guide navigation and persisted update preference menu changes preserve unsaved connection drafts')
    result = {'passed': True, 'synthetic_only': True, 'real_phone_contacted': False,
              'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'checks': checks}
    (output/'action_menu_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
