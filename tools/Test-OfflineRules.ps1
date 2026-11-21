<#
.SYNOPSIS
Compile the portable offline-package rules and check pin identity. No model download.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$python = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
& $python (Join-Path $root 'tests\offline_pins_test.py')
if ($LASTEXITCODE -ne 0) { throw 'Offline pin identity check failed.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC tools not found.' }
$out = Join-Path $root ('tmp\offline-rules-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($out) | Out-Null
$batch = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cd /d "$out"
cl /nologo /std:c++17 /MT /EHsc /W4 "$root\tests\offline_rules_test.cpp" /Fe:offline_rules_test.exe
if errorlevel 1 exit /b 1
offline_rules_test.exe
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $out 'test.cmd'), $batch, [Text.UTF8Encoding]::new($false))
& cmd /c (Join-Path $out 'test.cmd')
if ($LASTEXITCODE -ne 0) { throw 'Offline rules test failed.' }
