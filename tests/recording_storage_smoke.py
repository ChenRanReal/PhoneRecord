"""Verify calendar storage through actual loopback recordings and owned native GUI controls."""

import argparse
import csv
from datetime import datetime, timedelta
import hashlib
import json
from pathlib import Path
import tempfile

from product_smoke import App, wait_for, start_frame, emit_rtp, fingerprint
from desktop_smoke import USER, set_text, title, dismiss_dialog
from desktop_workflow import main_window, wav, cell, select_row
from recording_title_smoke import save_title
from compact_ui_smoke import group, choose, shown, bounds_fit
from localization_smoke import select_language, capture_page, resources


def record(app, session):
    """Create and stop a quiet synthetic RTP recording, then locate its nested sidecar."""
    reply = app.request(start_frame(session))
    assert reply['result'] == 'success'
    emit_rtp(int(reply['port']), [0xD5] * 100)
    app.stop_session(session)
    sidecar = wait_for(lambda: next((app.root/'recordings').rglob('*_' + fingerprint(session) + '.wav.quality.json'), None))
    metadata = wait_for(lambda: json.loads(sidecar.read_text(encoding='utf-8')) if sidecar.stat().st_size else None)
    return Path(str(sidecar).removesuffix('.quality.json')), metadata


def main():
    """Cover new destinations, legacy coexistence, nested actions, saved drafts and locale layout."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    resources()
    checks = []
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-storage-') as temporary:
        root = Path(temporary)
        for mode in ('flat', 'daily', 'weekly', 'custom'):
            app = App(root, exe, output, mode, gui=True)
            app.args += ['--capture-dir', str(output)]
            with app.config.open('a', encoding='utf-8') as config:
                config.write(f'record_folder_mode={mode}\nrecord_folder_pattern=自訂/{{yyyy}}/{{MM}}/{{dd}}\n')
            legacy = app.root/'recordings'/'20010101-010000_offline_7001_out.wav'
            legacy.parent.mkdir()
            wav(legacy)
            legacy_hash = hashlib.sha256(legacy.read_bytes()).hexdigest()
            with app:
                before = datetime.now()
                recording, metadata = record(app, 'storage-' + mode)
                relative = recording.relative_to(app.root/'recordings')
                # Derive the expected day from the recording's own start timestamp, allowing midnight.
                started = datetime.strptime(recording.name[:15], '%Y%m%d-%H%M%S')
                assert before - timedelta(seconds=2) <= started <= datetime.now() + timedelta(seconds=1)
                expected = {'flat': Path('.'), 'daily': Path(started.strftime('%Y-%m-%d')),
                            'weekly': Path((started - timedelta(days=started.weekday())).strftime('%Y-%m-%d')),
                            'custom': Path('自訂') / started.strftime('%Y') / started.strftime('%m') / started.strftime('%d')}[mode]
                assert relative.parent == expected, (relative, expected)
                assert metadata['source'] == 'test_or_other_peer'
                ledger = list(csv.reader((app.root/'recordings'/'calls.csv').open(encoding='utf-8', newline='')))
                assert ledger[-1][7].replace('\\', '/') == relative.as_posix()
                assert datetime.fromisoformat(ledger[-1][0]) == started
                assert recording.with_name(recording.name + '.quality.json').exists()

                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 101, 0)
                listing = USER.GetDlgItem(window, 610)
                assert USER.SendMessageW(listing, 0x1004, 0, 0) == 2
                assert cell(app.process.pid, listing, 0, 0).startswith(started.strftime('%Y-%m-%d'))
                assert cell(app.process.pid, listing, 1, 0).startswith('2001-01-01')
                select_row(listing, 0)
                original_audio = hashlib.sha256(recording.read_bytes()).hexdigest()
                save_title(app, window, '分日期錄音 ' + mode)
                assert json.loads(Path(str(recording) + '.title.json').read_text(encoding='utf-8'))['title'] == '分日期錄音 ' + mode
                set_text(USER.GetDlgItem(window, 210), '分日期錄音 ' + mode)
                assert USER.SendMessageW(listing, 0x1004, 0, 0) == 1
                select_row(listing, 0)
                USER.SendMessageW(window, 0x0111, 211, 0)
                wait_for(lambda: 'WAV_PLAYBACK_STARTED' in app.log() or USER.IsWindowEnabled(USER.GetDlgItem(window, 217)))
                USER.SendMessageW(window, 0x0111, 217, 0)
                assert hashlib.sha256(recording.read_bytes()).hexdigest() == original_audio
                assert hashlib.sha256(legacy.read_bytes()).hexdigest() == legacy_hash
            with app:
                window = main_window(app)
                USER.SendMessageW(window, 0x0111, 101, 0)
                listing = USER.GetDlgItem(window, 610)
                assert USER.SendMessageW(listing, 0x1004, 0, 0) == 2
                assert cell(app.process.pid, listing, 0, 1) == '分日期錄音 ' + mode
            checks.append(mode + ': real RTP destination, root ledger relative path, adjacent metadata, legacy coexistence, chronological sorting, Unicode rename/search/playback and restart')

        app = App(root, exe, output, 'drafts', gui=True)
        app.args += ['--capture-dir', str(output)]
        original = app.config.read_bytes()
        with app:
            window = main_window(app)
            for language in range(3):
                select_language(window, language)
                group(window, 2)
                choose(window, 418, 3)
                assert shown(window, 419)
                set_text(USER.GetDlgItem(window, 419), '週錄音/{weekStart}')
                bounds_fit(window)
                capture_page(window, output, 'storage_language_' + str(language), 3)
                choose(window, 418, 1)
                assert not shown(window, 419) and title(USER.GetDlgItem(window, 419)) == '週錄音/{weekStart}'
                choose(window, 418, 3)
            assert app.config.read_bytes() == original
            set_text(USER.GetDlgItem(window, 419), '../escape')
            group(window, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid)
            assert shown(window, 419) and app.config.read_bytes() == original
            set_text(USER.GetDlgItem(window, 419), '週錄音/{weekStart}')
            group(window, 0)
            USER.PostMessageW(window, 0x0111, 420, 0)
            dismiss_dialog(app.process.pid)
            saved = app.config.read_text(encoding='utf-8')
            assert 'record_folder_mode=custom' in saved and 'record_folder_pattern=週錄音/{weekStart}' in saved
            recording, _ = record(app, 'before-restart')
            assert recording.parent == app.root/'recordings'
        with app:
            recording, _ = record(app, 'after-restart')
            assert recording.parent.parent.name == '週錄音'
        checks.append('three-language custom pattern preview and visibility preserve drafts; invalid hidden values route to Preferences; save from Basic retains the template and applies only after restart')
    result = {'passed': True, 'synthetic_only': True, 'real_phone_contacted': False,
              'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'checks': checks}
    (output/'recording_storage_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
