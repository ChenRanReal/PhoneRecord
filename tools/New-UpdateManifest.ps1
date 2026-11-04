<#!
.SYNOPSIS
Sign a canonical update manifest for the already built executable. Intended for developer-triggered Actions releases.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Version, [string]$Executable = "$PSScriptRoot\..\cpp\PhoneRecord.exe")
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
    $manifest = "PhoneRecord-update-v1`nversion=$Version`narch=x86`nfile=PhoneRecord.exe`nsize=$($file.Length)`nsha256=$hash`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($manifest)
    $signature = $rsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $folder = $file.DirectoryName
    [IO.File]::WriteAllBytes((Join-Path $folder 'update-manifest.txt'), $bytes)
    [IO.File]::WriteAllText((Join-Path $folder 'update-manifest.sig'), [Convert]::ToBase64String($signature), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $folder 'PhoneRecord.exe.sha256'), "$hash  PhoneRecord.exe`n", [Text.UTF8Encoding]::new($false))
} finally {
    if ($private) { [Array]::Clear($private, 0, $private.Length) }
    $env:UPDATE_SIGNING_KEY = $null
    $rsa.Dispose()
}
