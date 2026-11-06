"""Exercise both complete installer plans with mocked transport and native processes."""
import json
import subprocess
import tempfile
from pathlib import Path
from product_smoke import startup_info


def main():
    """Verify selected assets, isolated markers and prepare jobs without downloading weights."""
    source = Path(__file__).parents[1]/'cpp/speech'
    script = (source/'qwen-install.ps1').read_text('utf-8')
    # Only this copied fixture substitutes external effects; the shipped script has no test bypass.
    mocks = r'''
$script:Downloads=@()
function Fetch($Url,$File,$Hash,$Algorithm,$Step,$Size=0) {
 $script:Downloads+=@{url=$Url;file=$File;hash=$Hash;step=$Step;size=$Size}
}
function FixtureNative {
 $global:LASTEXITCODE=0
 if($args.Count -gt 1 -and [string]$args[-1] -like '*prepare.json') {
  $job=Get-Content -LiteralPath $args[-1] -Raw -Encoding UTF8 | ConvertFrom-Json
  if($job.variant -ne $Variant -or $job.root -ne (Join-Path $Root 'qwen')) { throw 'Wrong model preparation job' }
 }
 return 'fixture native success'
}
'''
    script = script.replace('try {\n $Root=', mocks+'\ntry {\n $Root=', 1)
    native = "& (Join-Path $python 'python.exe')"
    assert script.count(native) == 3
    script = script.replace(native, 'FixtureNative')
    script += "\n[IO.File]::WriteAllText((Join-Path $Root 'fixture-downloads.json'),(ConvertTo-Json -InputObject $script:Downloads -Depth 5),$utf8)\n"
    with tempfile.TemporaryDirectory(prefix='PhoneRecord-qwen-installer-') as folder:
        base = Path(folder)
        for name in ('install.ps1','qwen-assets.json'):
            (base/name).write_bytes((source/name).read_bytes())
        (base/'qwen-install.ps1').write_text(script,'utf-8')
        root = base/'runtime'
        python = root/'qwen/python'
        (python/'Lib/site-packages/pip').mkdir(parents=True)
        (python/'python.exe').write_bytes(b'fixture; never executed')
        for variant, directory, marker, size in (
            ('0.6B','models','installed-v1',1576363223),
            ('1.7B','models-1.7b','installed-1.7b-v1',4087628216),
        ):
            result = subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
                '-File',str(base/'qwen-install.ps1'),'-Root',str(root),'-ProgressFile',str(root/'progress.json'),
                '-Variant',variant],capture_output=True,text=True,encoding='utf-8',errors='replace',
                startupinfo=startup_info(),creationflags=subprocess.CREATE_NO_WINDOW)
            assert result.returncode == 0, result.stdout+result.stderr
            downloads = json.loads((root/'fixture-downloads.json').read_text())
            models = [item for item in downloads if item['step'] == 'model']
            assert len(models) == 7 and sum(item['size'] for item in models) == size
            assert all(Path(item['file']).parent == root/'qwen'/directory for item in models)
            assert all(f'Qwen3-ASR-{variant}-hf/resolve/' in item['url'] for item in models)
            assert all(len(item['hash']) == 64 for item in models)
            assert variant in (root/'qwen'/marker).read_text()
            assert json.loads((root/'progress.json').read_text())['step'] == 'ready'
        assert '0.6B' in (root/'qwen/installed-v1').read_text()
        assert '1.7B' in (root/'qwen/installed-1.7b-v1').read_text()
    print('QWEN_INSTALLER_OK sizes=2 isolated_markers=1 pinned_assets=1 prepare_variant=1 no_network=1')


if __name__ == '__main__':
    main()
