<#!
.SYNOPSIS
Upload the DPAPI-protected publisher key directly to the GitHub Actions secret without printing plaintext.
.DESCRIPTION
Requires an existing Git Credential Manager login and Python PyNaCl. No release is created or triggered.
#>
[CmdletBinding()]
param([string]$Python = 'py', [string]$DependencyPath = '')
$ErrorActionPreference = 'Stop'
$keyPath = Join-Path $env:LOCALAPPDATA 'PhoneRecord-Publisher\update-signing.dpapi'
$private = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyPath), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
try {
    $payload = @{ privateKey = [Convert]::ToBase64String($private); dependencyPath = $DependencyPath } | ConvertTo-Json -Compress
    $payload | & $Python "$PSScriptRoot\set_update_secret.py"
    if ($LASTEXITCODE -ne 0) { throw 'GitHub Actions secret configuration failed. See the non-secret diagnostic above.' }
} finally {
    [Array]::Clear($private, 0, $private.Length)
    $payload = $null
}
