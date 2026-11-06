<# .SYNOPSIS
Run isolated x64 queue/cancellation regressions and synthetic JavaScript audio tests without models.
#>
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $root ('tmp\speech-native-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($out) | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 tools not found.' }
$batch=@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cd /d "$out"
cl /nologo /std:c++17 /MT /O2 /GS /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 "$root\tests\speech_native.cpp" /Fe:speech_native.exe /link /SUBSYSTEM:CONSOLE ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib
if errorlevel 1 exit /b 1
speech_native.exe
exit /b %errorlevel%
"@
$file=Join-Path $out 'test.cmd'
[IO.File]::WriteAllText($file,$batch,[Text.UTF8Encoding]::new($false))
& cmd /c $file
if ($LASTEXITCODE -ne 0) { throw 'Native speech regression failed.' }
& node --test (Join-Path $root 'tests\speech_worker.cjs')
if ($LASTEXITCODE -ne 0) { throw 'Synthetic speech worker regression failed.' }
$pythonCommand = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
& $pythonCommand (Join-Path $root 'tests\qwen_worker_test.py')
if ($LASTEXITCODE -ne 0) { throw 'Synthetic Qwen worker regression failed.' }
& $pythonCommand (Join-Path $root 'tests\qwen_installer_test.py')
if ($LASTEXITCODE -ne 0) { throw 'Qwen installer model isolation regression failed.' }
