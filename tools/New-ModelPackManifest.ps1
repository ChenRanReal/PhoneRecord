<#
.SYNOPSIS
Sign model-pack manifests for offline zips that the packager already wrote. Does not sign the executable manifest.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})$') { throw 'Expected a stable three-component version.' }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$header = [IO.File]::ReadAllText((Join-Path $root 'cpp\product_version.h'))
if ($header -notmatch ('#define PHONERECORD_VERSION "' + [regex]::Escape($Version) + '"')) { throw 'Tag and product version do not match.' }
$rules = [IO.File]::ReadAllText((Join-Path $root 'cpp\offline_rules.hpp'))
if ($rules -notmatch 'kGitHubReleaseAssetMaxBytes = (\d+)ULL') { throw 'Cannot read the GitHub asset limit.' }
$limit = [uint64]$Matches[1]
if (-not $env:UPDATE_SIGNING_KEY) { throw 'Configure the UPDATE_SIGNING_KEY repository secret before publishing.' }
$packs = @(
    @{ variant = 'offline-sensevoice'; zip = 'PhoneRecord-x64-offline-sensevoice.zip'; manifest = 'update-manifest-x64-offline-sensevoice' },
    @{ variant = 'offline-qwen06'; zip = 'PhoneRecord-x64-offline-qwen06.zip'; manifest = 'update-manifest-x64-offline-qwen06' },
    @{ variant = 'offline-qwen17'; zip = 'PhoneRecord-x64-offline-qwen17.zip'; manifest = 'update-manifest-x64-offline-qwen17' }
)
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new()
$rsa.PersistKeyInCsp = $false
$private = $null
try {
    $private = [Convert]::FromBase64String($env:UPDATE_SIGNING_KEY)
    $rsa.ImportCspBlob($private)
    $publicHeader = [IO.File]::ReadAllText((Join-Path $root 'cpp\update_public_key.hpp'))
    $public = [Convert]::ToBase64String($rsa.ExportCspBlob($false))
    if (-not $publicHeader.Contains('kUpdatePublicKey = "' + $public + '"')) { throw 'Signing secret does not match the pinned public key.' }
    $utf8 = [Text.UTF8Encoding]::new($false)
    foreach ($pack in $packs) {
        $zip = Join-Path $OutputDirectory $pack.zip
        if (-not [IO.File]::Exists($zip)) { continue }
        $info = Get-Item -LiteralPath $zip
        if ([uint64]$info.Length -ge $limit) { throw "$($pack.zip) exceeds the GitHub asset limit." }
        $fingerprintFile = Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($pack.zip) + '.fingerprint')
        $fingerprint = ([IO.File]::ReadAllText($fingerprintFile)).Trim()
        if ($fingerprint -notmatch '^[0-9a-f]{64}$') { throw "Missing fingerprint for $($pack.zip)." }
        $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest = "PhoneRecord-modelpack-v1`nversion=$Version`narch=x64`nvariant=$($pack.variant)`nfile=$($pack.zip)`nsize=$($info.Length)`nsha256=$sha`nfingerprint=$fingerprint`n"
        $bytes = $utf8.GetBytes($manifest)
        $signature = $rsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        [IO.File]::WriteAllBytes((Join-Path $OutputDirectory ($pack.manifest + '.txt')), $bytes)
        [IO.File]::WriteAllText((Join-Path $OutputDirectory ($pack.manifest + '.sig')), [Convert]::ToBase64String($signature), $utf8)
    }
} finally {
    if ($private) { [Array]::Clear($private, 0, $private.Length) }
    $env:UPDATE_SIGNING_KEY = $null
    $rsa.Dispose()
}
