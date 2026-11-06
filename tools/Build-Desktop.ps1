<# .SYNOPSIS
Build an isolated native desktop for x86 or x64 without replacing a running executable.
#>
[CmdletBinding()]
param([ValidateSet('x86','x64')][string]$Architecture='x86', [string]$OutputDirectory, [switch]$SpeechTesting)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root "tmp\desktop-$Architecture" }
$out=[IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($out) | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC tools not found.' }
$source=Join-Path $root 'cpp'
$manifest=Join-Path $source $(if ($Architecture -eq 'x64') {'phonerecord.x64.manifest'} else {'phonerecord.manifest'})
$define=if ($Architecture -eq 'x64') {'/d PHONERECORD_X64'} else {''}
$compilerDefine=if ($Architecture -eq 'x64') {'/DPHONERECORD_X64'} else {''}
if ($SpeechTesting) {
    if ($Architecture -ne 'x64') { throw 'Speech fixtures require x64.' }
    $compilerDefine += ' /DPHONERECORD_SPEECH_TESTING'
}
$name=if ($Architecture -eq 'x64') {'PhoneRecord-x64.exe'} else {'PhoneRecord.exe'}
$batch=@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvarsall.bat" $Architecture >nul
if errorlevel 1 exit /b 1
cd /d "$source"
rc /nologo /c65001 $define /fo "$out\phonerecord.res" phonerecord.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /MT /O2 /GS /EHsc /utf-8 /DUNICODE /D_UNICODE $compilerDefine /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 /Fo"$out\phonerecord.obj" phonerecord.cpp "$out\phonerecord.res" /Fe:"$out\$name" /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /MANIFEST:EMBED /MANIFESTINPUT:"$manifest" ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib
exit /b %errorlevel%
"@
$script=Join-Path $out 'build.cmd'
[IO.File]::WriteAllText($script,$batch,[Text.UTF8Encoding]::new($false))
& cmd /c $script
if ($LASTEXITCODE -ne 0) { throw "$Architecture build failed." }
Write-Output (Join-Path $out $name)
