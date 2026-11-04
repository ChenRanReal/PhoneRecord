<#!
.SYNOPSIS
Compile and execute native update regressions in an isolated temporary workspace; no publication or real phone access.
#>
[CmdletBinding()]
param([switch]$SkipTray)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x86 tools not found.' }
$workspace = Join-Path ([IO.Path]::GetTempPath()) ('PhoneRecord-update-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($workspace) | Out-Null
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(2048)
$rsa.PersistKeyInCsp = $false
try {
    $manifest = "PhoneRecord cross-runtime signing fixture`n"
    $signature = $rsa.SignData([Text.Encoding]::UTF8.GetBytes($manifest), [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $fixture = @{ manifest = $manifest; publicKey = [Convert]::ToBase64String($rsa.ExportCspBlob($false)); signature = [Convert]::ToBase64String($signature) } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $workspace 'interop.json'), $fixture, [Text.UTF8Encoding]::new($false))
} finally { $rsa.Dispose() }
$batch = Join-Path $workspace 'test.cmd'
$testOption = if ($SkipTray) { '--skip-tray' } else { '' }
$command = "@echo off`r`ncall `"$installation\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul`r`nif errorlevel 1 exit /b 1`r`ncd /d `"$workspace`"`r`ncl /nologo /std:c++17 /MT /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 `"$root\tests\update_native.cpp`" /Fe:update_native.exe /link /SUBSYSTEM:CONSOLE ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib ole32.lib`r`nif errorlevel 1 exit /b 1`r`nupdate_native.exe `"$workspace`" $testOption`r`nexit /b %errorlevel%`r`n"
[IO.File]::WriteAllText($batch, $command, [Text.UTF8Encoding]::new($false))
try {
    & cmd /c $batch
    if ($LASTEXITCODE -ne 0) { throw 'Native update regression failed.' }
    if (-not (Test-Path -LiteralPath (Join-Path $workspace 'native-success.txt'))) { throw 'Native tests did not produce completion evidence.' }
    Write-Output ([IO.File]::ReadAllText((Join-Path $workspace 'native-success.txt')).Trim())
} finally {
    $resolved = [IO.Path]::GetFullPath($workspace)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('PhoneRecord-update-tests-')) { throw 'Refusing to clean an unexpected test workspace.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
