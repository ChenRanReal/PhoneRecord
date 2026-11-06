<#
.SYNOPSIS
Build and verify native calendar storage rules in an isolated output directory.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x86 tools not found.' }
$workspace = Join-Path $root ('tmp\storage-native-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($workspace) | Out-Null
$batch = Join-Path $workspace 'test.cmd'
$command = @"
@echo off
call "$installation\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1
cd /d "$workspace"
cl /nologo /std:c++17 /MT /O2 /GS /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 "$root\tests\recording_storage_native.cpp" /Fe:recording_storage_native.exe /link /SUBSYSTEM:CONSOLE ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib
if errorlevel 1 exit /b 1
recording_storage_native.exe
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($batch, $command, [Text.UTF8Encoding]::new($false))
& cmd /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Recording storage native regressions failed.' }
