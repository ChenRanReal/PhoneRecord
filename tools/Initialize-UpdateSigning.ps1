<#!
.SYNOPSIS
Initialize or reuse a publisher RSA key protected by Windows DPAPI; write only its public key to source.
.DESCRIPTION
Run once before the first updater-enabled release. The private key remains encrypted outside the repository.
Use Set-UpdateSigningSecret.ps1 to upload it directly to GitHub Actions without printing it.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$keyDirectory = Join-Path $env:LOCALAPPDATA 'PhoneRecord-Publisher'
$keyPath = Join-Path $keyDirectory 'update-signing.dpapi'
$headerPath = Join-Path $PSScriptRoot '..\cpp\update_public_key.hpp'
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(3072)
$rsa.PersistKeyInCsp = $false
try {
    if (Test-Path -LiteralPath $keyPath) {
        $protected = [IO.File]::ReadAllBytes($keyPath)
        $private = [Security.Cryptography.ProtectedData]::Unprotect($protected, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
        $rsa.ImportCspBlob($private)
    } else {
        $existing = [IO.File]::ReadAllText($headerPath)
        if ($existing -notmatch 'kUpdatePublicKey = "";') { throw 'A public key already exists. Restore its original encrypted private key; do not rotate it silently.' }
        [IO.Directory]::CreateDirectory($keyDirectory) | Out-Null
        $private = $rsa.ExportCspBlob($true)
        $protected = [Security.Cryptography.ProtectedData]::Protect($private, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
        [IO.File]::WriteAllBytes($keyPath, $protected)
    }
    $public = [Convert]::ToBase64String($rsa.ExportCspBlob($false))
    $existing = [IO.File]::ReadAllText($headerPath)
    if ($existing -notmatch 'kUpdatePublicKey = "";' -and -not $existing.Contains('kUpdatePublicKey = "' + $public + '"')) { throw 'Local private key does not match the pinned public key. Refusing automatic rotation.' }
    $header = "#pragma once`n`n/// Pinned publisher RSA public key (CryptoAPI PUBLICKEYBLOB); private key stays outside source.`nstatic const char* const kUpdatePublicKey = `"$public`";`n"
    [IO.File]::WriteAllText($headerPath, $header, [Text.UTF8Encoding]::new($false))
    Write-Output 'Publisher public key initialized; the private key is protected with Windows DPAPI.'
} finally {
    if ($private) { [Array]::Clear($private, 0, $private.Length) }
    $rsa.Dispose()
}
