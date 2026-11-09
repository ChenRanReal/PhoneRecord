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


def page_bounds(owner, page, live):
    """Both task cards and actions fit inside the established minimum window."""
    USER.GetDpiForWindow.argtypes=[w.HWND];USER.GetDpiForWindow.restype=w.UINT
    USER.SetWindowPos.argtypes=[w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT]
    dpi=USER.GetDpiForWindow(owner)
    USER.SetWindowPos(owner,None,0,0,(980*dpi+95)//96,(760*dpi+95)//96,0x0016)
    client=w.RECT();USER.GetClientRect(page,ctypes.byref(client))
    for id in ([1,41] if live else [8,35,34,36,32])+[2,3,4,5,12,30,31]:
        child=USER.GetDlgItem(page,id);assert child,id
        area=w.RECT();USER.GetWindowRect(child,ctypes.byref(area))
        a,b=w.POINT(area.left,area.top),w.POINT(area.right,area.bottom)
        USER.ScreenToClient(page,ctypes.byref(a));USER.ScreenToClient(page,ctypes.byref(b))
        assert 0<=a.x<b.x<=client.right,(id,a.x,b.x,client.right)
        assert 0<=a.y<b.y<=client.bottom,(id,a.y,b.y,client.bottom)


def choose(page,id,index):
    """Perform a native combo selection, including its notification."""
    child=USER.GetDlgItem(page,id);USER.SendMessageW(child,0x014E,index,0)
    USER.SendMessageW(page,0x0111,(1<<16)|id,child)


def main():
    """Verify two visible modules with task-local settings and direct historical recording selection."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--x86',type=Path,required=True);parser.add_argument('--x64',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--failure-exe',type=Path)
    args=parser.parse_args();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    USER.SetThreadDpiAwarenessContext.argtypes=[w.HANDLE];USER.SetThreadDpiAwarenessContext.restype=w.HANDLE
    USER.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4));resources()
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-modules-ui-') as folder:
        for arch,exe in [('x86',args.x86.resolve()),('x64',args.x64.resolve())]:
            app=App(Path(folder),exe,output,arch,gui=True);app.args+=['--capture-dir',str(output)]
            source=app.root/'recordings'/'20261106-120000_in_100_200_fixture.wav'
            source.parent.mkdir(parents=True,exist_ok=True);wav(source)
            for suffix,text in [('.transcript','SenseVoice saved'),('.qwen3asr.transcript','Qwen 0.6B saved'),('.qwen3asr-1.7b.transcript','Qwen 1.7B saved')]:
                Path(str(source)+suffix+'.txt').write_text(text,'utf-8')
            try:
                app.launch();owner=main_window(app)
                if arch=='x86':
                    assert not USER.GetDlgItem(owner,107) and not USER.GetDlgItem(owner,109)
                    continue
                assert not USER.GetDlgItem(owner,436) and not USER.GetDlgItem(owner,228) and not USER.GetDlgItem(owner,229)
                for locale in range(3):
                    select_language(owner,locale)
                    USER.SendMessageW(owner,0x0111,103,0)
                    for tab in (432,433,434):
                        USER.SendMessageW(owner,0x0111,tab,0)
                        assert not USER.GetDlgItem(owner,436)
                    for obsolete in (436,228,229,226,227):
                        USER.SendMessageW(owner,0x0111,obsolete,0)
                        hidden=USER.GetDlgItem(owner,960)
                        assert not hidden or not USER.GetWindowLongW(hidden,-16)&0x10000000
                    for live,id in [(True,107),(False,109)]:
                        USER.SendMessageW(owner,0x0111,id,0);page=USER.GetDlgItem(owner,960)
                        assert USER.GetParent(page)==owner
                        assert bool(USER.GetDlgItem(page,1))==live
                        assert bool(USER.GetDlgItem(page,8))!=live
                        if not live:
                            assert USER.SendMessageW(USER.GetDlgItem(page,8),0x0146,0,0)>=2
                            assert USER.SendMessageW(USER.GetDlgItem(page,34),0x0146,0,0)==8
                            assert USER.SendMessageW(USER.GetDlgItem(page,35),0x0146,0,0)==1
                            for index,text in enumerate(['SenseVoice saved','Qwen 0.6B saved','Qwen 1.7B saved']):
                                choose(page,34,index);assert text in title(USER.GetDlgItem(page,30)), (index,text,title(USER.GetDlgItem(page,30)),title(USER.GetDlgItem(page,35)))
                        page_bounds(owner,page,live)
                        USER.SendMessageW(owner,0x0111,9900,0)
                        page_index=6 if live else 8
                        with Image.open(output/f'PhoneRecord-{page_index}.bmp') as picture:picture.convert('RGB').save(output/f'module-{id}-{locale}.png')
                        USER.SendMessageW(page,0x0111,12,0)
                        assert title(USER.GetDlgItem(page,10)) and (USER.GetWindowLongW(USER.GetDlgItem(page,10),-16)&0x10000000), (live,locale,title(USER.GetDlgItem(page,10)),(USER.GetWindowLongW(USER.GetDlgItem(page,10),-16)&0x10000000))
                        USER.SendMessageW(page,0x0111,12,0)
                        assert (USER.GetWindowLongW(USER.GetDlgItem(page,30),-16)&0x10000000)
                USER.SendMessageW(owner,0x0111,107,0);page=USER.GetDlgItem(owner,960)
                choose(page,2,2);choose(page,3,1)
                enable=USER.GetDlgItem(page,1);USER.SendMessageW(enable,0x00F1,1,0);USER.SendMessageW(page,0x0111,1,enable)
                USER.SendMessageW(owner,0x0111,109,0);page=USER.GetDlgItem(owner,960)
                assert USER.SendMessageW(USER.GetDlgItem(page,2),0x0147,0,0)==0
                history=USER.GetDlgItem(page,45)
                assert USER.SendMessageW(history,0x00F0,0,0)==0
                USER.SendMessageW(history,0x00F1,1,0);USER.SendMessageW(page,0x0111,45,history)
                choose(page,2,4);choose(page,8,1)
                USER.SendMessageW(owner,0x0111,107,0);page=USER.GetDlgItem(owner,960)
                assert USER.SendMessageW(USER.GetDlgItem(page,2),0x0147,0,0)==2
                assert USER.SendMessageW(USER.GetDlgItem(page,3),0x0147,0,0)==1
                enable=USER.GetDlgItem(page,1);USER.SendMessageW(enable,0x00F1,0,0);USER.SendMessageW(page,0x0111,1,enable)
                settings=json.loads(Path(str(app.config)+'.speech').read_text('utf-8'))
                assert settings['liveEnabled']=='0' and settings['historyEnabled']=='1' and settings['enabled']=='1'
                assert settings['liveLanguage']=='en' and settings['language']=='ja' and settings['model']=='qwen17'
                assert not windows(app.process.pid,'PhoneRecordSpeechWindow')
            finally:app.shutdown()
        if args.failure_exe:
            app=App(Path(folder),args.failure_exe.resolve(),output,'failure',gui=True)
            runtime=Path(str(app.config)+'.speech-test-runtime');runtime.mkdir()
            Path(str(app.config)+'.speech').write_text(json.dumps(dict(enabled='1',liveEnabled='0',model='qwen17')),'utf-8')
            (runtime/'install-error.json').write_text(json.dumps(dict(step='http_failed',phase='model',model='qwen17',resource='model.safetensors',httpStatus='503',detail='Synthetic server unavailable')),'utf-8')
            try:
                app.launch();owner=main_window(app)
                for locale in range(3):
                    select_language(owner,locale);USER.SendMessageW(owner,0x0111,109,0)
                    page=USER.GetDlgItem(owner,960);report=title(USER.GetDlgItem(page,10))
                    assert '503' in report and 'model.safetensors' in report
                    assert USER.GetWindowLongW(USER.GetDlgItem(page,10),-16)&0x10000000
                    USER.SendMessageW(page,0x0111,9,0)
                    assert USER.GetWindowLongW(USER.GetDlgItem(page,30),-16)&0x10000000
                    USER.SendMessageW(owner,0x0111,107,0);page=USER.GetDlgItem(owner,960)
                    assert not USER.GetWindowLongW(USER.GetDlgItem(page,10),-16)&0x10000000
            finally:app.shutdown()
    print('SPEECH_MODULE_UI_OK x86_gated=1 locales=3 modules=2 settings_isolated=1 results=3 no_downloads=1')


if __name__=='__main__':
    main()
