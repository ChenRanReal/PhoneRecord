<#!
.SYNOPSIS
Sign a canonical update manifest for the already built executable. Intended for developer-triggered Actions releases.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Version, [string]$Executable = "$PSScriptRoot\..\cpp\PhoneRecord.exe", [ValidateSet('x86','x64')][string]$Architecture='x86')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count -gt 0) { throw 'Expected a stable three-component version, each between 0 and 65535.' }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionHeader = [IO.File]::ReadAllText((Join-Path $root 'cpp\product_version.h'))
if ($versionHeader -notmatch ('#define PHONERECORD_VERSION "' + [regex]::Escape($Version) + '"')) { throw 'Tag and product version do not match.' }
if (-not $env:UPDATE_SIGNING_KEY) { throw 'Configure the UPDATE_SIGNING_KEY repository secret before publishing.' }
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new()
$rsa.PersistKeyInCsp = $false
try {
    $private = [Convert]::FromBase64String($env:UPDATE_SIGNING_KEY)
    $rsa.ImportCspBlob($private)
    $publicHeader = [IO.File]::ReadAllText((Join-Path $root 'cpp\update_public_key.hpp'))
    $public = [Convert]::ToBase64String($rsa.ExportCspBlob($false))
    if (-not $publicHeader.Contains('kUpdatePublicKey = "' + $public + '"')) { throw 'Signing secret does not match the pinned public key.' }
    $file = Get-Item -LiteralPath $Executable
    if ($file.Length -le 0 -or $file.Length -gt 64MB) { throw 'Unexpected executable size.' }
    if ($file.VersionInfo.ProductVersion -ne $Version) { throw 'Executable version does not match the release tag.' }
    $hash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    $image=[IO.File]::ReadAllBytes($file.FullName)
    $pe=[BitConverter]::ToInt32($image,0x3c)
    $expected=if ($Architecture -eq 'x64') {0x8664} else {0x014c}
    if ($pe -lt 0 -or $pe + 6 -gt $image.Length -or [BitConverter]::ToUInt16($image,$pe+4) -ne $expected) { throw 'Executable architecture does not match manifest.' }
    $manifest = "PhoneRecord-update-v1`nversion=$Version`narch=$Architecture`nfile=PhoneRecord.exe`nsize=$($file.Length)`nsha256=$hash`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($manifest)
    $signature = $rsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $folder = $file.DirectoryName
    $manifestName=if ($Architecture -eq 'x64') {'update-manifest-x64'} else {'update-manifest'}
    $assetName=if ($Architecture -eq 'x64') {'PhoneRecord-x64.exe'} else {'PhoneRecord.exe'}
    [IO.File]::WriteAllBytes((Join-Path $folder "$manifestName.txt"), $bytes)
    [IO.File]::WriteAllText((Join-Path $folder "$manifestName.sig"), [Convert]::ToBase64String($signature), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $folder "$assetName.sha256"), "$hash  $assetName`n", [Text.UTF8Encoding]::new($false))
} finally {
    if ($private) { [Array]::Clear($private, 0, $private.Length) }
    $env:UPDATE_SIGNING_KEY = $null
    $rsa.Dispose()
}
