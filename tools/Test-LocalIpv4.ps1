<#!
.SYNOPSIS
Verify local-address selection and native settings drafts using synthetic adapter evidence.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x86 tools not found.' }
$workspace = Join-Path ([IO.Path]::GetTempPath()) ('PhoneRecord-local-ip-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($workspace) | Out-Null
$batch = Join-Path $workspace 'test.cmd'
$command = "@echo off`r`ncall `"$installation\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul`r`nif errorlevel 1 exit /b 1`r`ncd /d `"$workspace`"`r`ncl /nologo /std:c++17 /MT /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 `"$root\tests\local_ipv4_native.cpp`" /Fe:local_ipv4_native.exe /link /SUBSYSTEM:CONSOLE ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib ole32.lib`r`nif errorlevel 1 exit /b 1`r`nlocal_ipv4_native.exe`r`nexit /b %errorlevel%`r`n"
[IO.File]::WriteAllText($batch, $command, [Text.UTF8Encoding]::new($false))
try {
    & cmd /c $batch
    if ($LASTEXITCODE -ne 0) { throw 'Local IPv4 regressions failed.' }
} finally {
    $resolved = [IO.Path]::GetFullPath($workspace)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('PhoneRecord-local-ip-tests-')) { throw 'Unexpected test workspace.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
