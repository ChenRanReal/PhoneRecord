"""Validate owned offline x86/x64 interfaces; no model installation, phone or real recordings."""
import argparse
import ctypes
from ctypes import wintypes as w
import json
from pathlib import Path
import tempfile
from PIL import Image
from product_smoke import App, wait_for
from desktop_smoke import USER, windows, title
from desktop_workflow import main_window, wav, select_row
from localization_smoke import select_language, resources, capture_page


def capture(window, destination):
    """Capture only the owned speech window via PrintWindow, without reading the desktop."""
    gdi = ctypes.WinDLL('gdi32', use_last_error=True)
    for name, result, args in [
        ('CreateCompatibleDC', w.HDC, [w.HDC]),
        ('CreateCompatibleBitmap', w.HBITMAP, [w.HDC, ctypes.c_int, ctypes.c_int]),
        ('SelectObject', w.HANDLE, [w.HDC, w.HANDLE]),
        ('DeleteObject', w.BOOL, [w.HANDLE]), ('DeleteDC', w.BOOL, [w.HDC]),
    ]:
        fn = getattr(gdi, name); fn.restype = result; fn.argtypes = args
    USER.GetDC.argtypes = [w.HWND]; USER.GetDC.restype = w.HDC
    USER.ReleaseDC.argtypes = [w.HWND, w.HDC]
    USER.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]; USER.PrintWindow.restype = w.BOOL
    rect = w.RECT(); USER.GetClientRect(window, ctypes.byref(rect))
    dc = USER.GetDC(window); memory = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, rect.right, rect.bottom); old = gdi.SelectObject(memory, bitmap)
    try:
        assert USER.PrintWindow(window, memory, 1)
        class Header(ctypes.Structure):
            _fields_ = [('size', w.DWORD), ('width', w.LONG), ('height', w.LONG), ('planes', w.WORD),
                        ('bits', w.WORD), ('compression', w.DWORD), ('image', w.DWORD), ('x', w.LONG),
                        ('y', w.LONG), ('used', w.DWORD), ('important', w.DWORD)]
        header = Header(ctypes.sizeof(Header), rect.right, -rect.bottom, 1, 32, 0, 0, 0, 0, 0, 0)
        data = ctypes.create_string_buffer(rect.right * rect.bottom * 4)
        gdi.SelectObject(memory, old)
        gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
        assert gdi.GetDIBits(memory, bitmap, 0, rect.bottom, data, ctypes.byref(header), 0)
        Image.frombuffer('RGB', (rect.right, rect.bottom), data, 'raw', 'BGRX', 0, 1).save(destination)
    finally:
        gdi.SelectObject(memory, old); gdi.DeleteObject(bitmap); gdi.DeleteDC(memory); USER.ReleaseDC(window, dc)


def page_bounds(owner, page):
    """Keep settings, progress and actions inside the existing minimum-size main window."""
    USER.GetDpiForWindow.argtypes = [w.HWND]
    USER.GetDpiForWindow.restype = w.UINT
    USER.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    dpi = USER.GetDpiForWindow(owner)
    USER.SetWindowPos(owner, None, 0, 0, (980*dpi+95)//96, (760*dpi+95)//96, 0x0016)
    client = w.RECT(); USER.GetClientRect(page, ctypes.byref(client))
    for control in (1, 2, 3, 4, 5, 7, 8, 40, 42, 43):
        child = USER.GetDlgItem(page, control)
        area = w.RECT(); USER.GetWindowRect(child, ctypes.byref(area))
        start, end = w.POINT(area.left,area.top), w.POINT(area.right,area.bottom)
        USER.ScreenToClient(page,ctypes.byref(start)); USER.ScreenToClient(page,ctypes.byref(end))
        assert 0 <= start.x < end.x <= client.right, control
        assert 0 <= start.y < end.y <= client.bottom, control


def main():
    """Verify architecture gating, translated controls, immediate preferences and opening/closing."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--x86', type=Path, required=True)
    parser.add_argument('--x64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--failure-exe', type=Path)
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    USER.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
    USER.SetThreadDpiAwarenessContext.restype = w.HANDLE
    USER.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    resources()
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-speech-ui-') as folder:
        for arch, exe in [('x86', args.x86.resolve()), ('x64', args.x64.resolve())]:
            app = App(Path(folder), exe, output, arch, gui=True)
            app.args += ['--capture-dir', str(output)]
            try:
                app.launch(); owner = main_window(app)
                if arch == 'x86':
                    assert not USER.GetDlgItem(owner, 436)
                    assert not USER.GetDlgItem(owner, 107)
                    USER.SendMessageW(owner, 0x0111, 436, 0)
                    assert not windows(app.process.pid, 'PhoneRecordSpeechWindow')
                    continue
                assert USER.GetDlgItem(owner, 436)
                assert USER.GetWindowLongW(USER.GetDlgItem(owner, 107), -16) & 0x10000000
                for language, expected in [(0, '开启'), (1, '開啟'), (2, 'Enable')]:
                    select_language(owner, language)
                    # The ordinary sidebar entry is always visible, including while the feature is off.
                    for page in range(5):
                        USER.SendMessageW(owner, 0x0111, 100 + page, 0)
                        assert USER.GetWindowLongW(USER.GetDlgItem(owner, 107), -16) & 0x10000000
                    USER.SendMessageW(owner, 0x0111, 100, 0)
                    capture_page(owner, output, f'main-speech-entry-{language}', 0)
                    USER.SendMessageW(owner, 0x0111, 107, 0)
                    speech = wait_for(lambda: USER.GetDlgItem(owner, 960))
                    assert USER.GetParent(speech) == owner
                    assert USER.GetWindowLongW(speech, -16) & 0x40000000
                    assert not windows(app.process.pid, 'PhoneRecordSpeechWindow')
                    enable = USER.GetDlgItem(speech, 1)
                    assert expected in title(enable)
                    assert USER.SendMessageW(enable, 0x00F0, 0, 0) == 0
                    page_bounds(owner, speech)
                    USER.SendMessageW(owner, 0x0111, 9900, 0)
                    with Image.open(output / 'PhoneRecord-6.bmp') as source_image:
                        source_image.convert('RGB').save(output / f'speech-{language}.png')
                    USER.SendMessageW(enable, 0x00F1, 1, 0)
                    USER.SendMessageW(speech, 0x0111, 1, enable)
                    settings = json.loads(Path(str(app.config) + '.speech').read_text(encoding='utf-8'))
                    assert settings['enabled'] == '1'
                    USER.SendMessageW(owner, 0x0111, 9900, 0)
                    with Image.open(output / 'PhoneRecord-6.bmp') as source_image:
                        source_image.convert('RGB').save(output / f'speech-enabled-{language}.png')
                    USER.SendMessageW(speech, 0x0111, 7, 0)
                    USER.SendMessageW(speech, 0x0111, 7, 0)
                    # No install click is sent; enablement alone must not launch a speech child.
                    USER.SendMessageW(enable, 0x00F1, 0, 0)
                    USER.SendMessageW(speech, 0x0111, 1, enable)
                    assert json.loads(Path(str(app.config) + '.speech').read_text())['enabled'] == '0'
                    USER.SendMessageW(owner, 0x0111, 101, 0)
                    assert not (USER.GetWindowLongW(speech, -16) & 0x10000000)
                source = app.root / 'recordings' / '2026-11-06' / '20261106-120000_in_100_200_fixture.wav'
                source.parent.mkdir(parents=True, exist_ok=True); wav(source)
                text = '合成測試文字。Synthetic transcript.\n' * 4000
                Path(str(source) + '.transcript.txt').write_bytes(text.encode('utf-8'))
                Path(str(source) + '.transcript.json').write_text('{"status":"complete"}', encoding='utf-8')
                USER.SendMessageW(owner, 0x0111, 101, 0); USER.SendMessageW(owner, 0x0111, 214, 0)
                select_row(USER.GetDlgItem(owner, 610), 0); USER.SendMessageW(owner, 0x0111, 226, 0)
                speech = wait_for(lambda: USER.GetDlgItem(owner, 960))
                assert USER.GetParent(speech) == owner
                viewer = USER.GetDlgItem(speech, 30)
                assert USER.SendMessageW(viewer, 0x000E, 0, 0) > 65536
                assert '合成測試文字' in title(viewer)
                assert '\r\n' in title(viewer)
                USER.SendMessageW(owner, 0x0111, 9900, 0)
                with Image.open(output / 'PhoneRecord-6.bmp') as source_image:
                    source_image.convert('RGB').save(output / 'speech-result.png')
                Path(str(source) + '.qwen3asr.transcript.txt').write_text('Qwen second version',encoding='utf-8')
                version = USER.GetDlgItem(speech,34)
                USER.SendMessageW(version,0x014E,1,0); USER.SendMessageW(speech,0x0111,(1<<16)|34,version)
                assert 'Qwen second version' in title(viewer)
                assert USER.SendMessageW(version,0x0146,0,0) == 3
                Path(str(source) + '.qwen3asr-1.7b.transcript.txt').write_text('Qwen 1.7B independent version',encoding='utf-8')
                USER.SendMessageW(version,0x014E,2,0); USER.SendMessageW(speech,0x0111,(1<<16)|34,version)
                assert 'Qwen 1.7B independent version' in title(viewer)
                assert Path(str(source)+'.qwen3asr.transcript.txt').read_text() == 'Qwen second version'
                USER.SendMessageW(version,0x014E,0,0); USER.SendMessageW(speech,0x0111,(1<<16)|34,version)
                assert 'Synthetic transcript' in title(viewer)
                USER.SendMessageW(owner,0x0111,228,0)
                speech = USER.GetDlgItem(owner,960)
                assert USER.GetDlgItem(speech,30) and not USER.IsWindowEnabled(USER.GetDlgItem(speech,34))
                assert not USER.IsWindowEnabled(USER.GetDlgItem(speech,32))
                USER.SendMessageW(owner,0x0111,9900,0)
                with Image.open(output / 'PhoneRecord-6.bmp') as picture: picture.convert('RGB').save(output / 'speech-live-waiting.png')
                USER.SendMessageW(owner,0x0111,229,0)
                speech = USER.GetDlgItem(owner,960)
                model = USER.GetDlgItem(speech,8)
                USER.SendMessageW(model,0x014E,1,0); USER.SendMessageW(speech,0x0111,(1<<16)|8,model)
                assert json.loads(Path(str(app.config)+'.speech').read_text())['model'] == 'qwen'
                USER.SendMessageW(owner,0x0111,9900,0)
                with Image.open(output / 'PhoneRecord-6.bmp') as picture: picture.convert('RGB').save(output / 'speech-qwen-settings.png')
                assert USER.SendMessageW(model,0x0146,0,0) == 3
                enable = USER.GetDlgItem(speech,1)
                USER.SendMessageW(enable,0x00F1,1,0); USER.SendMessageW(speech,0x0111,1,enable)
                for locale in range(3):
                    select_language(owner,locale)
                    USER.SendMessageW(model,0x014E,2,0); USER.SendMessageW(speech,0x0111,(1<<16)|8,model)
                    assert json.loads(Path(str(app.config)+'.speech').read_text())['model'] == 'qwen17'
                    page_bounds(owner,speech)
                    USER.SendMessageW(owner,0x0111,9900,0)
                    with Image.open(output / 'PhoneRecord-6.bmp') as picture: picture.convert('RGB').save(output / f'speech-qwen17-settings-{locale}.png')
                USER.SendMessageW(owner, 0x0111, 101, 0)
                assert app.process.poll() is None
            finally:
                app.shutdown()
        if args.failure_exe:
            app = App(Path(folder),args.failure_exe.resolve(),output,'failure',gui=True)
            app.args += ['--capture-dir',str(output)]
            fixture = Path(str(app.config) + '.speech-test-runtime'); fixture.mkdir()
            Path(str(app.config) + '.speech').write_text('{"enabled":"1","language":"auto","source":"huggingface.co"}',encoding='utf-8')
            (fixture / 'install-error.json').write_text(json.dumps({'step':'http_failed','phase':'model','operation':'download','percent':'0','resource':'model.int8.onnx','origin':'huggingface.co','httpStatus':'503','nativeCode':'ProtocolError','detail':'Synthetic HTTP 503 test response. No model was downloaded.'}),encoding='utf-8')
            try:
                app.launch(); owner = main_window(app)
                for locale in range(3):
                    select_language(owner,locale); USER.SendMessageW(owner,0x0111,436,0)
                    page = USER.GetDlgItem(owner,960); page_bounds(owner,page)
                    wait_for(lambda: USER.GetWindowLongW(USER.GetDlgItem(page,9),-16) & 0x10000000)
                    USER.SendMessageW(owner,0x0111,9900,0)
                    with Image.open(output / 'PhoneRecord-6.bmp') as picture: picture.convert('RGB').save(output / f'speech-failure-{locale}.png')
                    USER.SendMessageW(page,0x0111,9,0)
                    report = title(USER.GetDlgItem(page,10))
                    assert all(value in report for value in ('model.int8.onnx','huggingface.co','503','ProtocolError'))
                    choose = USER.GetDlgItem(page,3); USER.SendMessageW(choose,0x014E,1,0); USER.SendMessageW(page,0x0111,(1<<16)|3,choose)
                    assert '503' in title(USER.GetDlgItem(page,10))
                    USER.SendMessageW(owner,0x0111,9900,0)
                    with Image.open(output / 'PhoneRecord-6.bmp') as picture: picture.convert('RGB').save(output / f'speech-failure-details-{locale}.png')
                    USER.SendMessageW(page,0x0111,9,0)
            finally: app.shutdown()
    print('SPEECH_UI_OK x86_gated=1 locales=3 preference_isolation=1 no_models=1')


if __name__ == '__main__':
    main()
