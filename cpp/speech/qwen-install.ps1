<# .SYNOPSIS
Install a pinned private Windows CPU Qwen3-ASR environment on explicit request.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Root,[Parameter(Mandatory)][string]$ProgressFile,
 [ValidateSet('huggingface.co','hf-mirror.com')][string]$Source='huggingface.co',
 [ValidateSet('0.6B','1.7B')][string]$Variant='0.6B',[switch]$RuntimeOnly)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
$utf8=[Text.UTF8Encoding]::new($false)
$script:Model=if($Variant -eq '1.7B'){'qwen17'}else{'qwen'}
$script:Phase='check'; $script:Resource=''; $script:Origin=''; $script:Operation='check'; $script:NativeDetail=''; $script:ExitCode=''
# Load helper declarations only; do not execute SenseVoice installation.
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'install.ps1'),[ref]$null,[ref]$null)
foreach($fn in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
 . ([scriptblock]::Create($fn.Extent.Text))
}
try {
 $Root=[IO.Path]::GetFullPath($Root)
 $qwen=Join-Path $Root 'qwen'
 $modelDirectory=if($Variant -eq '1.7B'){'models-1.7b'}else{'models'}
 $marker=if($Variant -eq '1.7B'){'installed-1.7b-v1'}else{'installed-v1'}
 foreach($dir in @($qwen,(Join-Path $qwen 'cache'),(Join-Path $qwen $modelDirectory),(Join-Path $qwen 'wheels'))) { [IO.Directory]::CreateDirectory($dir) | Out-Null }
 Remove-Item -LiteralPath (Join-Path $qwen $marker) -Force -ErrorAction SilentlyContinue
 Publish 'check'
 if ([Version](Get-CimInstance Win32_OperatingSystem).Version -lt [Version]'10.0') { throw 'unsupported_os' }
 $requiredSpace=if($Variant -eq '1.7B'){12GB}else{6GB}
 if (([IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root))).AvailableFreeSpace -lt $requiredSpace) { throw 'disk' }
 $assets=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'qwen-assets.json') -Raw -Encoding UTF8 | ConvertFrom-Json
 $python=Join-Path $qwen 'python'
 $zip=Join-Path $qwen 'cache\python.zip'
 Fetch $assets.python.url $zip $assets.python.sha256 'SHA256' 'runtime' $assets.python.size
 Add-Type -AssemblyName System.IO.Compression.FileSystem
 if (-not [IO.File]::Exists((Join-Path $python 'python.exe'))) { [IO.Compression.ZipFile]::ExtractToDirectory($zip,$python) }
 $site=Join-Path $python 'Lib\site-packages'; [IO.Directory]::CreateDirectory($site) | Out-Null
 [IO.File]::WriteAllText((Join-Path $python 'python312._pth'),"python312.zip`n.`nLib/site-packages`nimport site`n",$utf8)
 $pip=Join-Path $qwen ('wheels\'+$assets.pip.file)
 Fetch $assets.pip.url $pip $assets.pip.sha256 'SHA256' 'runtime' $assets.pip.size
 if (-not [IO.Directory]::Exists((Join-Path $site 'pip'))) { [IO.Compression.ZipFile]::ExtractToDirectory($pip,$site) }
 foreach($asset in $assets.wheels) { Fetch $asset.url (Join-Path $qwen ('wheels\'+$asset.file)) $asset.sha256 'SHA256' 'runtime' }
 $script:Operation='install'; Publish 'runtime'
 $env:PIP_DISABLE_PIP_VERSION_CHECK='1'; $env:HF_HUB_OFFLINE='1'; $env:HF_HUB_DISABLE_TELEMETRY='1'; $env:TRANSFORMERS_OFFLINE='1'
 $ErrorActionPreference='Continue'; $global:LASTEXITCODE=$null
 $output=& (Join-Path $python 'python.exe') -m pip install --no-index --no-cache-dir --find-links (Join-Path $qwen 'wheels') transformers==5.13.0 torch==2.9.1+cpu numpy==2.2.6 soundfile==0.13.1 2>&1
 $script:ExitCode=if($null -eq $LASTEXITCODE){'not-started'}else{[string]$LASTEXITCODE}; $ErrorActionPreference='Stop'
 $script:NativeDetail=($output | Out-String)
 if($script:ExitCode -ne '0'){throw 'runtime_failed'}
 $ErrorActionPreference='Continue'; $global:LASTEXITCODE=$null
 $output=& (Join-Path $python 'python.exe') (Join-Path $PSScriptRoot 'qwen-worker.py') --check-runtime 2>&1
 $script:ExitCode=if($null -eq $LASTEXITCODE){'not-started'}else{[string]$LASTEXITCODE}; $ErrorActionPreference='Stop'
 $script:NativeDetail=($output | Out-String)
 if($script:ExitCode -ne '0'){throw 'runtime_failed'}
 if($RuntimeOnly){ Publish 'ready' 100; exit 0 }
 $modelAssets=if($Variant -eq '1.7B'){$assets.models17}else{$assets.models}
 foreach($asset in $modelAssets) {
  Fetch ($asset.url.Replace('huggingface.co',$Source)) (Join-Path (Join-Path $qwen $modelDirectory) $asset.file) $asset.sha256 'SHA256' 'model' $asset.size
 }
 $script:Phase='load'; $script:Operation='load'; Publish 'load'
 $job=Join-Path $qwen 'prepare.json'
 [IO.File]::WriteAllText($job,(@{root=$qwen;variant=$Variant;prepare=$true;language='auto';progress=$ProgressFile}|ConvertTo-Json -Compress),$utf8)
 $ErrorActionPreference='Continue'; $global:LASTEXITCODE=$null
 $output=& (Join-Path $python 'python.exe') (Join-Path $PSScriptRoot 'qwen-worker.py') $job 2>&1
 $script:ExitCode=if($null -eq $LASTEXITCODE){'not-started'}else{[string]$LASTEXITCODE}; $ErrorActionPreference='Stop'
 $script:NativeDetail=($output | Out-String)
 if($script:ExitCode -ne '0'){throw 'inference_failed'}
 [IO.File]::WriteAllText((Join-Path $qwen $marker),('qwen3-asr-'+$Variant+';transformers5.13.0;torch2.9.1+cpu'),$utf8)
 Remove-Item -LiteralPath (Join-Path $Root 'install-error.json') -Force -ErrorAction SilentlyContinue
 Publish 'ready' 100
} catch { ReportFailure $_.Exception; exit 1 }
