"""Validate provider adapters and lifecycle using fake executables; no real installs or accounts."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid


def main():
    """Exercise installer dispatch, normalized model sources, bounded output and final responses."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    root = repository/'tmp'/('ai-cli-worker-'+uuid.uuid4().hex)
    root.mkdir(parents=True)
    fixture = args.fixture.resolve()
    worker = repository/'cpp/ai_cli_worker.ps1'
    environment = os.environ.copy()
    environment['USERPROFILE'] = str(root/'home')
    environment['LOCALAPPDATA'] = str(root/'local')
    environment['APPDATA'] = str(root/'roaming')
    environment['PATH'] = os.environ['SystemRoot']+r'\System32;'+os.environ['SystemRoot']+r'\System32\WindowsPowerShell\v1.0'
    (root/'home/.qwen').mkdir(parents=True)
    (root/'home/.qwen/settings.json').write_text(json.dumps({'modelProviders':{'synthetic':[{'id':'synthetic-model','apiKey':'DO_NOT_PERSIST'}]}}),encoding='utf-8')
    trae = root/'roaming/trae_cli'; trae.mkdir(parents=True)
    (trae/'trae_cli.yaml').write_text('model:\n  name: synthetic-model\n',encoding='utf-8')
    def run(provider, action, **extra):
        """Invoke the production worker with isolated user directories and explicit fake CLI."""
        work = root/(provider+'-'+action+'-'+uuid.uuid4().hex); work.mkdir()
        request = work/'request.json'
        request.write_text(json.dumps(dict(provider=provider,action=action,executable=str(fixture),model='synthetic-model',timeout=10,**extra)),encoding='utf-8')
        result = subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(worker),'-Request',str(request)],env=environment,capture_output=True,timeout=35)
        report = work/'observation.json'
        observation = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
        assert b'DO_NOT_PERSIST' not in result.stdout+result.stderr
        assert 'DO_NOT_PERSIST' not in json.dumps(observation)
        return result, observation, work
    count = 0
    for provider in ('claude','codex','cursor','antigravity','codebuddy','qwen','kimi','qoder','trae'):
        result, report, _ = run(provider,'probe')
        assert result.returncode == 0 and report['step']=='installed',(provider,result.stderr,report)
        result, report, _ = run(provider,'models')
        assert result.returncode == 0 and report['step']=='models_ready',(provider,result.stderr,report)
        assert report['models'] and report['source'],(provider,report)
        count += 2
    for provider in ('codex','cursor','antigravity','qwen','kimi','qoder','trae'):
        input_file=root/(provider+'-input.txt');input_file.write_text('Synthetic conversation: 使用者需求',encoding='utf-8')
        result, _, _=run(provider,'run',input=str(input_file))
        assert result.returncode==0 and b'Synthetic' in result.stdout,(provider,result.stderr)
        count+=1
    # Existing installations are probed and never reinstalled or fetched.
    result, report, _=run('claude','install')
    assert result.returncode==0 and report['step']=='installed'
    count+=1
    # Execute the production install path with a mocked official download and isolated installation.
    work=root/'install-mock';work.mkdir();request=work/'request.json'
    request.write_text(json.dumps(dict(provider='claude',action='install',executable='')),encoding='utf-8')
    launcher=work/'mock-download.ps1'
    destination=root/'home/.local/bin/claude.exe'
    quoted=lambda text: str(text).replace("'","''")
    source="[IO.Directory]::CreateDirectory('"+quoted(destination.parent)+"') | Out-Null;[IO.File]::Copy('"+quoted(fixture)+"','"+quoted(destination)+"',$true)"
    launcher.write_text("function Invoke-WebRequest { param($Uri,[switch]$UseBasicParsing,$TimeoutSec) if($Uri -ne 'https://claude.ai/install.ps1'){throw 'Unexpected source'};return @{StatusCode=200;Content='"+quoted(source)+"'} }; & '"+quoted(worker)+"' -Request '"+quoted(request)+"'",encoding='utf-8')
    result=subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(launcher)],env=environment,capture_output=True,timeout=35)
    report=json.loads((work/'observation.json').read_text(encoding='utf-8'))
    assert result.returncode==0 and report['step']=='installed' and destination.exists(),(result.stderr,report)
    assert (work/'installer.sha256').exists()
    count+=1
    print(f'AI_CLI_WORKER_OK {count} checks; synthetic adapters, model sources and mocked official installation')
    return 0


if __name__=='__main__':
    raise SystemExit(main())
