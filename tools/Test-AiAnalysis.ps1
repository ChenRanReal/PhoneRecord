<# .SYNOPSIS
Run isolated x64 analysis contracts using synthetic stdio only; no real CLI accounts or calls.
#>
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $root ('tmp\ai-native-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($out) | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs) {throw 'MSVC tools not found.'}
$batch=@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cd /d "$out"
cl /nologo /std:c++17 /MT /EHsc /utf-8 "$root\tests\ai_cli_fixture.cpp" /Fe:ai_cli_fixture.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /MT /O2 /GS /EHsc /utf-8 /DUNICODE /D_UNICODE /DPHONERECORD_X64 /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 "$root\tests\ai_native.cpp" /Fe:ai_native.exe /link /SUBSYSTEM:CONSOLE ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib
if errorlevel 1 exit /b 1
ai_native.exe "$out\ai_cli_fixture.exe"
exit /b %errorlevel%
"@
$script=Join-Path $out 'test.cmd'
[IO.File]::WriteAllText($script,$batch,[Text.UTF8Encoding]::new($false))
& cmd /c $script
if($LASTEXITCODE -ne 0) {throw 'AI analysis native regression failed.'}
$python=if(Get-Command py -ErrorAction SilentlyContinue){'py'}else{'python'}
& $python (Join-Path $root 'tests\ai_cli_worker_test.py') --fixture (Join-Path $out 'ai_cli_fixture.exe')
if($LASTEXITCODE -ne 0){throw 'AI CLI worker regression failed.'}
