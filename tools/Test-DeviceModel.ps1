<#
.SYNOPSIS
Build and run the device-model loader, matcher and Fanvil X3S contract tests.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x86 tools not found.' }
$workspace = Join-Path $root ('tmp\device-model-' + [Guid]::NewGuid().ToString('N'))
$plugins = Join-Path $workspace 'plugins'
[IO.Directory]::CreateDirectory($plugins) | Out-Null
$batch = Join-Path $workspace 'test.cmd'
$command = @"
@echo off
call "$installation\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1
cd /d "$workspace"
cl /nologo /std:c++17 /MT /LD /EHsc /utf-8 /D_WIN32_WINNT=0x0601 "$root\cpp\models\sample_template_plugin.cpp" /Fe:"$plugins\sample.dll"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /MT /LD /EHsc /utf-8 /D_WIN32_WINNT=0x0601 "$root\cpp\models\fanvil_x3s_plugin.cpp" /Fe:"$plugins\fanvil.dll"
if errorlevel 1 exit /b 1
cl /nologo /std:c17 /MT /LD /utf-8 "$root\tests\device_model_widget.c" /Fe:"$plugins\widget.dll"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /MT /EHsc /utf-8 /D_WIN32_WINNT=0x0601 "$root\tests\device_model_test.cpp" /Fe:device_model_test.exe
if errorlevel 1 exit /b 1
device_model_test.exe --plugins "$plugins"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($batch, $command, [Text.UTF8Encoding]::new($false))
& cmd /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Device model tests failed.' }
