"""Exercise the owned native analysis page with synthetic calls and a local fake CLI only."""
import argparse
import ctypes
import json
from pathlib import Path
import tempfile
from ctypes import wintypes as w
from PIL import Image

from product_smoke import App, wait_for
from desktop_smoke import USER, title, set_text, dismiss_dialog
from desktop_workflow import main_window, wav, select_row
from localization_smoke import select_language, resources
from speech_ui_smoke import capture


def choose(page, control, index):
    """Use the real combo notification path rather than changing internal page state."""
    child = USER.GetDlgItem(page, control)
    USER.SendMessageW(child, 0x014E, index, 0)
    USER.SendMessageW(page, 0x0111, (1 << 16) | control, child)


def main():
    """Validate profile separation, defaults, call binding, preview and result persistence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    assert resources() >= 216
    with tempfile.TemporaryDirectory(prefix='phonerecord-ai-ui-') as temporary:
        root = Path(temporary)
        app = App(root, args.exe.resolve(), output, 'ai-ui', gui=True)
        app.args += ['--capture-dir', str(output)]
        project = app.root / 'project'; project.mkdir()
        (project / 'README.md').write_text('Synthetic project: assignment flow.', encoding='utf-8')
        profiles = [
            dict(id='cli-one', kind='cli', name='Synthetic CLI', type='stdin', path=str(args.fixture.resolve()), timeout='10'),
            dict(id='cli-two', kind='cli', name='Other CLI', type='codebuddy', path='codebuddy.exe', timeout='300'),
            dict(id='cli-life', kind='cli', name='Lifecycle CLI', type='claude', path=str(args.fixture.resolve()), timeout='10'),
            dict(id='prompt-one', kind='prompt', name='Custom requirements', body='Extract confirmed requirements.'),
            dict(id='project-one', kind='project', name='Synthetic iLab', path=str(project), cli='cli-one', prompt='prompt-one', references='README.md'),
        ]
        Path(str(app.config)+'.ai-profiles').write_text(''.join(json.dumps(p, ensure_ascii=False)+'\n' for p in profiles), encoding='utf-8')
        source = app.root / 'recordings' / '2026-11-06' / '20261106-120000_in_100_200_fixture.wav'
        source.parent.mkdir(parents=True)
        wav(source, 2)
        for suffix, text in [('.transcript.json', '需要修改排配'), ('.qwen3asr.transcript.json', '不需要修改排配')]:
            Path(str(source)+suffix).write_text(json.dumps(dict(status='complete', text=text, segments=[dict(start=0,end=2,text=text)]), ensure_ascii=False), encoding='utf-8')
        try:
            app.launch(); owner = main_window(app)
            select_language(owner, 0)
            USER.SendMessageW(owner, 0x0111, 108, 0)
            page = wait_for(lambda: USER.GetDlgItem(owner, 961))
            assert 'CLI' in title(USER.GetDlgItem(page, 11))
            # Verify all settings categories stay independent and retain stable references.
            for tab in (11, 12, 13):
                USER.SendMessageW(page, 0x0111, tab, 0)
                assert USER.GetDlgItem(page, 20) and USER.GetDlgItem(page, 60)
            USER.SendMessageW(page,0x0111,11,0)
            enable=USER.GetDlgItem(page,15)
            assert USER.SendMessageW(enable,0x00F0,0,0)==0
            USER.SendMessageW(enable,0x00F1,1,0);USER.SendMessageW(page,0x0111,15,enable)
            assert USER.SendMessageW(USER.GetDlgItem(page,24),0x018B,0,0)==9
            choose(page,20,2)
            # The install button probes the already-present synthetic EXE; no vendor download occurs.
            USER.SendMessageW(page,0x0111,72,0)
            # Installation of an already-present executable also loads that tool's model list.
            wait_for(lambda: USER.SendMessageW(USER.GetDlgItem(page,63),0x0146,0,0)>=4,20)
            set_text(USER.GetDlgItem(page,63),'sonnet')
            USER.PostMessageW(page,0x0111,22,0);dismiss_dialog(app.process.pid)
            saved=[json.loads(line) for line in Path(str(app.config)+'.ai-profiles').read_text(encoding='utf-8').splitlines()]
            selected=next(p for p in saved if p['id']=='cli-life')
            assert selected['model']=='sonnet' and not USER.GetDlgItem(page,75)
            assert Path(str(app.config)+'.ai-profiles.models').exists()
            USER.SendMessageW(owner, 0x0111, 101, 0)
            select_row(USER.GetDlgItem(owner, 610), 0)
            USER.SendMessageW(owner, 0x0111, 231, 0)
            page = USER.GetDlgItem(owner, 961)
            choose(page, 30, 1)
            assert USER.SendMessageW(USER.GetDlgItem(page,31),0x0147,0,0) == 1
            assert USER.SendMessageW(USER.GetDlgItem(page,32),0x0147,0,0) == 4
            # Preview both sources and exact reference data without invoking a CLI.
            USER.SendMessageW(page,0x0111,52,0)
            wait_for(lambda: '需要修改排配' in title(USER.GetDlgItem(page,80)))
            preview = title(USER.GetDlgItem(page,80))
            assert '不需要修改排配' in preview and 'README.md' in preview
            assert json.loads(Path(str(source)+'.ai-project.json').read_text())['project']=='project-one'
            USER.SendMessageW(page,0x0111,51,0)
            wait_for(lambda: 'Synthetic analysis' in title(USER.GetDlgItem(page,80)))
            assert Path(str(source)+'.ai-latest.json').exists()
            choose(page,31,3)
            USER.SendMessageW(page,0x0111,51,0)
            wait_for(lambda: USER.SendMessageW(USER.GetDlgItem(page,35),0x0146,0,0)==2)
            assert 'Lifecycle CLI' in title(USER.GetDlgItem(page,35))
            choose(page,35,1)
            assert 'Synthetic CLI' in title(USER.GetDlgItem(page,35))
            # Preserve results and controls in each supported UI locale.
            for index, language in enumerate(('zh-CN','zh-TW','en')):
                select_language(owner,index)
                wait_for(lambda: title(USER.GetDlgItem(page,51)) == ('AI 分析' if index<2 else 'Analyze'))
                USER.SendMessageW(owner,0x0111,9900,0)
                with Image.open(output/'PhoneRecord-7.bmp') as image:
                    image.convert('RGB').save(output/f'ai-analysis-{language}.png')
                for tab,name in ((11,'cli'),(12,'projects'),(13,'prompts')):
                    USER.SendMessageW(page,0x0111,tab,0)
                    USER.SendMessageW(owner,0x0111,9900,0)
                    with Image.open(output/'PhoneRecord-7.bmp') as image:
                        image.convert('RGB').save(output/f'ai-{name}-{language}.png')
                USER.SendMessageW(page,0x0111,10,0)
            assert source.exists()
        finally:
            app.shutdown()
    print('AI_UI_OK catalog install_probe model_refresh default_model cached_models multi_cli_results three_languages')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
