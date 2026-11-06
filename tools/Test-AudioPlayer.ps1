<#
.SYNOPSIS
Build and run synthetic streaming-player regressions; optionally exercise real Windows audio with silence.
#>
[CmdletBinding()]
param([switch]$WithOutput)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x86 tools not found.' }
$workspace = Join-Path ([IO.Path]::GetTempPath()) ('PhoneRecord-audio-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($workspace) | Out-Null
$batch = Join-Path $workspace 'test.cmd'
$lines = @('@echo off', "call `"$installation\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$workspace`"")
$targets = @('audio_player_native')
if ($WithOutput) { $targets += 'audio_player_output' }
foreach ($target in $targets) {
    $lines += "cl /nologo /std:c++17 /MT /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE `"$root\tests\$target.cpp`" /Fe:$target.exe /link winmm.lib"
    $lines += 'if errorlevel 1 exit /b 1', "$target.exe", 'if errorlevel 1 exit /b 1'
}
$lines += 'exit /b 0'
[IO.File]::WriteAllText($batch, ($lines -join "`r`n"), [Text.UTF8Encoding]::new($false))
try {
    & cmd /c $batch
    if ($LASTEXITCODE -ne 0) { throw 'Audio player regressions failed.' }
} finally {
    $resolved = [IO.Path]::GetFullPath($workspace)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('PhoneRecord-audio-tests-')) { throw 'Unexpected test workspace.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
