"""Verify compact settings using only an owned offline GUI and synthetic files."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import tempfile
from ctypes import wintypes as w

from product_smoke import App, wait_for, http_get
from desktop_smoke import USER, title, set_text, windows, dismiss_dialog
from desktop_workflow import main_window, wav, select_row
from localization_smoke import select_language, capture_page, select_help, resources
from recording_title_smoke import editor, close_editor


def shown(window, control):
    """Read the child visibility bit even though the owned test parent is hidden."""
    child = USER.GetDlgItem(window, control)
    return bool(child and USER.GetWindowLongW(child, -16) & 0x10000000)


def group(window, index):
    """Select the real settings navigation and category buttons."""
    USER.SendMessageW(window, 0x0111, 103, 0)
    USER.SendMessageW(window, 0x0111, 432 + index, 0)


def choose(window, control, index):
    """Select a native combo entry and send its ordinary change notification."""
    child = USER.GetDlgItem(window, control)
    USER.SendMessageW(child, 0x014E, index, 0)
    USER.SendMessageW(window, 0x0111, (1 << 16) | control, child)


def bounds_fit(window):
    """Keep every displayed settings edit and action inside the minimum-size client."""
    USER.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
    USER.SetThreadDpiAwarenessContext.restype = w.HANDLE
    USER.GetDpiForWindow.argtypes = [w.HWND]
    USER.GetDpiForWindow.restype = w.UINT
    USER.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    previous = USER.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    try:
        dpi = USER.GetDpiForWindow(window)
        rect = w.RECT()
        USER.GetWindowRect(window, ctypes.byref(rect))
        USER.SetWindowPos(window, None, rect.left, rect.top, (980 * dpi + 95) // 96, (760 * dpi + 95) // 96, 0x0014)
        client = w.RECT()
        USER.GetClientRect(window, ctypes.byref(client))
        for control in (*range(400, 420), 420, 421, 422, 423, 424, 432, 433, 434, 435, 722, 729, 730, 731, 732, 733):
            if not shown(window, control):
                continue
            area = w.RECT()
            USER.GetWindowRect(USER.GetDlgItem(window, control), ctypes.byref(area))
            top_left, bottom_right = w.POINT(area.left, area.top), w.POINT(area.right, area.bottom)
            USER.ScreenToClient(window, ctypes.byref(top_left))
            USER.ScreenToClient(window, ctypes.byref(bottom_right))
            assert 0 <= top_left.x < bottom_right.x <= client.right, control
            assert 0 <= top_left.y < bottom_right.y <= client.bottom, control
    finally:
        USER.SetThreadDpiAwarenessContext(previous)


def main():
    """Cover visibility, draft retention, validation routing, diagnostics and recording actions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-compact-ui-') as temporary:
        app = App(Path(temporary), exe, output, 'compact', gui=True)
        app.args += ['--capture-dir', str(output)]
        folder = app.root/'recordings'
        folder.mkdir()
        recording = folder/'20261106-001000_offline_offline_out_s-1234-abcd.wav'
        wav(recording)
        before_audio = hashlib.sha256(recording.read_bytes()).hexdigest()
        original = app.config.read_bytes()
        with app:
            window = main_window(app)
            group(window, 0)
            assert all(shown(window, x) for x in (400, 402, 403, 410, 424, 729))
            assert all(not shown(window, x) for x in (401, 404, 405, 406, 407, 408, 409, 411, 412, 413, 414, 415, 416, 417))
            assert not USER.GetDlgItem(window, 425)
            checks.append('basic settings show only essential connection fields and retain one-click setup')

            group(window, 1)
            assert all(shown(window, x) for x in (401, 404, 405, 406, 407, 408, 409, 412, 413, 416))
            assert not shown(window, 417)
            choose(window, 416, 1)
            assert shown(window, 417)
            set_text(USER.GetDlgItem(window, 417), '02-AA-BB-CC-DD-01')
            choose(window, 416, 0)
            assert not shown(window, 417) and title(USER.GetDlgItem(window, 417)) == '02-AA-BB-CC-DD-01'
            set_text(USER.GetDlgItem(window, 404), '192.0.2.55')
            choose(window, 412, 3)  # 120 seconds, stored without resetting hidden controls.
            group(window, 2)
            assert all(shown(window, x) for x in (411, 414, 415))
            assert not shown(window, 404)
            choose(window, 411, 1)
            USER.SendMessageW(USER.GetDlgItem(window, 414), 0x00F1, 1, 0)
            checks.append('advanced values and DHCP MAC drafts remain intact; fixed mode hides MAC; preferences have their own category')

            for language in range(3):
                select_language(window, language)
                for category in range(3):
                    group(window, category)
                    bounds_fit(window)
                    capture_page(window, output, f'language_{language}_category_{category}', 3)
                assert title(USER.GetDlgItem(window, 404)) == '192.0.2.55'
                assert title(USER.GetDlgItem(window, 417)) == '02-AA-BB-CC-DD-01'
                assert app.config.read_bytes() == original
            checks.append('three-language categories fit the minimum window and switching never saves or clears connection/preference drafts')

            set_text(USER.GetDlgItem(window, 404), '')
            group(window, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid, 'advertise_ip')
            assert shown(window, 404) and not shown(window, 400)
            assert app.config.read_bytes() == original
            set_text(USER.GetDlgItem(window, 404), '192.0.2.55')
            checks.append('validation reveals the category containing an invalid hidden field without writing configuration')

            group(window, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid)
            saved = app.config.read_text(encoding='utf-8')
            assert 'advertise_ip=192.0.2.55' in saved and 'health_interval=120' in saved
            assert 'close_action=minimize' in saved and 'tray_notifications=1' in saved
            assert json.loads(http_get(app.event, '/health')[1])['control']
            checks.append('saving Basic retains hidden advanced/preferences values and leaves running listeners intact')

            USER.SendMessageW(window, 0x0111, 100, 0)
            assert shown(window, 502) and not USER.GetDlgItem(window, 503)
            USER.SendMessageW(window, 0x0111, 502, 0)
            assert shown(window, 801)
            checks.append('overview has one combined check entry leading to the detailed inspection report')

            USER.SendMessageW(window, 0x0111, 101, 0)
            assert all(not USER.GetDlgItem(window, x) for x in (212, 213, 215, 221))
            listing = USER.GetDlgItem(window, 610)
            select_row(listing, 0)
            dialog = editor(app, window, f2=True)
            close_editor(dialog)
            assert hashlib.sha256(recording.read_bytes()).hexdigest() == before_audio
            USER.PostMessageW(window, 0x007B, listing, -1)
            popup = wait_for(lambda: next(iter(windows(app.process.pid, '#32768')), None))
            USER.ShowWindow(popup, 0)
            USER.PostMessageW(window, 0x001F, 0, 0)  # WM_CANCELMODE, only this owned menu.
            wait_for(lambda: not windows(app.process.pid, '#32768'))
            capture_page(window, output, 'recordings', 1)
            checks.append('secondary toolbar buttons are removed; the native context menu opens and F2 rename remains available without changing audio')

    result = {'passed': True, 'synthetic_only': True, 'real_phone_contacted': False,
              'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'checks': checks}
    (output/'compact_ui_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
