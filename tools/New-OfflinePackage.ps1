<#
.SYNOPSIS
Build one offline x64 installer zip. Model bytes are downloaded into the cache, never committed.
.DESCRIPTION
sensevoice and qwen06 may be published when the finished zip is under the GitHub 2 GiB asset limit.
qwen17 weights alone exceed that limit, so -Publish writes a skip file and does not upload them.
The zip contains the already built executable, install-variant.txt, licenses and bundled-speech.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('sensevoice','qwen06','qwen17')][string]$Variant,
    [Parameter(Mandatory)][string]$Executable,
    [string]$OutputDirectory,
    [string]$CacheDirectory,
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'tmp\offline' }
if (-not $CacheDirectory) { $CacheDirectory = Join-Path $root 'tmp\model-cache' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
$utf8 = [Text.UTF8Encoding]::new($false)
$rules = [IO.File]::ReadAllText((Join-Path $root 'cpp\offline_rules.hpp'))
if ($rules -notmatch 'kGitHubReleaseAssetMaxBytes = (\d+)ULL') { throw 'Cannot read the GitHub asset limit from offline_rules.hpp.' }
$limit = [uint64]$Matches[1]
$pins = foreach ($match in [regex]::Matches($rules, '\{"(?<model>sense|qwen|qwen17)", "(?<path>[^"]+)", (?<size>\d+)ULL, "(?<sha>[0-9a-f]{64})"\}')) {
    [pscustomobject]@{ model = $match.Groups['model'].Value; path = $match.Groups['path'].Value; size = [uint64]$match.Groups['size'].Value; sha = $match.Groups['sha'].Value }
}
if ($pins.Count -lt 17) { throw 'Speech pin table is incomplete.' }
$model = @{ sensevoice = 'sense'; qwen06 = 'qwen'; qwen17 = 'qwen17' }[$Variant]
$variantId = @{ sensevoice = 'offline-sensevoice'; qwen06 = 'offline-qwen06'; qwen17 = 'offline-qwen17' }[$Variant]
$zipName = @{ sensevoice = 'PhoneRecord-x64-offline-sensevoice.zip'; qwen06 = 'PhoneRecord-x64-offline-qwen06.zip'; qwen17 = 'PhoneRecord-x64-offline-qwen17.zip' }[$Variant]
$pinnedBytes = [uint64]0
foreach ($pin in $pins) { if ($pin.model -eq $model) { $pinnedBytes += $pin.size } }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Write-Skip([string]$Reason) {
    $name = [IO.Path]::GetFileNameWithoutExtension($zipName) + '.skipped.txt'
    [IO.File]::WriteAllText((Join-Path $OutputDirectory $name), $Reason + "`n", $utf8)
    Write-Output $Reason
}
if ($Publish -and $pinnedBytes -ge $limit) {
    Write-Skip "$variantId is not a GitHub Release asset: pinned weights are $pinnedBytes bytes and the per-asset limit is $limit."
    exit 0
}
$executableItem = Get-Item -LiteralPath $Executable
if ($executableItem.Length -le 0 -or $executableItem.Length -gt 64MB) { throw 'Unexpected executable size.' }
$image = [IO.File]::ReadAllBytes($executableItem.FullName)
$pe = [BitConverter]::ToInt32($image, 0x3c)
if ($pe -lt 0 -or $pe + 6 -gt $image.Length -or [BitConverter]::ToUInt16($image, $pe + 4) -ne 0x8664) { throw 'Offline packages require the x64 executable.' }
$free = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($CacheDirectory)).AvailableFreeSpace
$need = if ($Variant -eq 'sensevoice') { 2GB } else { 8GB }
if ($free -lt $need) {
    $reason = "$variantId needs $need free bytes on the cache drive and found $free."
    if ($Publish -and $Variant -ne 'sensevoice') { Write-Skip $reason; exit 0 }
    throw $reason
}
function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Test-Ready([string]$Tree) {
    foreach ($pin in $pins) {
        if ($pin.model -ne $model) { continue }
        $file = Join-Path $Tree ($pin.path -replace '/', '\')
        if (-not [IO.File]::Exists($file)) { return $false }
        if ([uint64]([IO.FileInfo]$file).Length -ne $pin.size) { return $false }
        if ((Get-Sha256 $file) -ne $pin.sha) { return $false }
    }
    $runtime = if ($model -eq 'sense') { @('node-v24.11.0-win-x64\node.exe', 'runtime\node_modules\sherpa-onnx-node\package.json', 'installed-v1') }
        elseif ($model -eq 'qwen') { @('qwen\python\python.exe', 'qwen\installed-v1') }
        else { @('qwen\python\python.exe', 'qwen\installed-1.7b-v1') }
    foreach ($relative in $runtime) { if (-not [IO.File]::Exists((Join-Path $Tree $relative))) { return $false } }
    return $true
}
$build = Join-Path $CacheDirectory "$Variant-root"
[IO.Directory]::CreateDirectory($build) | Out-Null
if (-not (Test-Ready $build)) {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $progress = Join-Path $CacheDirectory "$Variant-progress.json"
    if ($Variant -eq 'sensevoice') {
        & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $root 'cpp\speech\install.ps1') -Root $build -ProgressFile $progress -Source huggingface.co
    } else {
        $qwenVariant = if ($Variant -eq 'qwen17') { '1.7B' } else { '0.6B' }
        & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $root 'cpp\speech\qwen-install.ps1') -Root $build -ProgressFile $progress -Source huggingface.co -Variant $qwenVariant
    }
    if ($LASTEXITCODE -ne 0) {
        $errorFile = Join-Path $build 'install-error.json'
        $detail = if ([IO.File]::Exists($errorFile)) { [IO.File]::ReadAllText($errorFile) } else { "exit $LASTEXITCODE" }
        throw "Offline installer preparation failed for ${Variant}: $detail"
    }
    if (-not (Test-Ready $build)) { throw "Prepared $Variant tree does not match the compiled pins." }
}
$folderName = [IO.Path]::GetFileNameWithoutExtension($zipName)
$stage = Join-Path $OutputDirectory $folderName
$bundle = Join-Path $stage 'bundled-speech'
if ([IO.Directory]::Exists($stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
[IO.Directory]::CreateDirectory($bundle) | Out-Null
Copy-Item -LiteralPath $executableItem.FullName -Destination (Join-Path $stage 'PhoneRecord-x64.exe')
[IO.File]::WriteAllText((Join-Path $stage 'install-variant.txt'), "PhoneRecord-variant-v1`nvariant=$variantId`n", $utf8)
# Traditional Chinese filenames are built from code points so Windows PowerShell 5.1 does not depend on the script code page.
$docLeaf = -join (@(0x96E2, 0x7DDA, 0x5B89, 0x88DD, 0x5305) | ForEach-Object { [char]$_ }) + '.md'
$readmeLeaf = -join (@(0x96E2, 0x7DDA, 0x5B89, 0x88DD, 0x8AAA, 0x660E) | ForEach-Object { [char]$_ }) + '.md'
Copy-Item -LiteralPath (Join-Path (Join-Path $root 'docs') $docLeaf) -Destination (Join-Path $stage $readmeLeaf)
if ($model -eq 'sense') {
    foreach ($name in @('node-v24.11.0-win-x64', 'runtime', 'models')) {
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination (Join-Path $bundle $name) -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $build 'installed-v1') -Destination (Join-Path $bundle 'installed-v1')
} else {
    $qwenStage = Join-Path $bundle 'qwen'
    [IO.Directory]::CreateDirectory($qwenStage) | Out-Null
    Copy-Item -LiteralPath (Join-Path $build 'qwen\python') -Destination (Join-Path $qwenStage 'python') -Recurse
    $models = if ($model -eq 'qwen17') { 'models-1.7b' } else { 'models' }
    $marker = if ($model -eq 'qwen17') { 'installed-1.7b-v1' } else { 'installed-v1' }
    Copy-Item -LiteralPath (Join-Path $build "qwen\$models") -Destination (Join-Path $qwenStage $models) -Recurse
    Copy-Item -LiteralPath (Join-Path $build "qwen\$marker") -Destination (Join-Path $qwenStage $marker)
}
$senseFlag = if ($model -eq 'sense') { '1' } else { '0' }
$qwenFlag = if ($model -eq 'qwen') { '1' } else { '0' }
$qwen17Flag = if ($model -eq 'qwen17') { '1' } else { '0' }
[IO.File]::WriteAllText((Join-Path $bundle 'bundle.txt'), "PhoneRecord-bundle-v1`nsense=$senseFlag`nqwen=$qwenFlag`nqwen17=$qwen17Flag`n", $utf8)
Copy-Item -LiteralPath (Join-Path $root 'cpp\speech\NOTICE.md') -Destination (Join-Path $bundle 'NOTICE.md')
Copy-Item -LiteralPath (Join-Path $root 'cpp\speech\licenses') -Destination (Join-Path $bundle 'licenses') -Recurse
$list = New-Object System.Collections.Generic.List[string]
foreach ($pin in ($pins | Where-Object model -eq $model | Sort-Object path)) {
    $file = Join-Path $bundle ($pin.path -replace '/', '\')
    if ((Get-Sha256 $file) -ne $pin.sha -or [uint64]([IO.FileInfo]$file).Length -ne $pin.size) { throw "Staged pin mismatch: $($pin.path)" }
    $list.Add("$($pin.path) $($pin.size) $($pin.sha)")
}
$fileList = ($list -join "`n") + "`n"
[IO.File]::WriteAllText((Join-Path $bundle 'bundle-files.txt'), $fileList, $utf8)
$fingerprint = Get-Sha256 (Join-Path $bundle 'bundle-files.txt')
[IO.File]::WriteAllText((Join-Path $bundle 'fingerprint.txt'), $fingerprint + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $OutputDirectory ($folderName + '.fingerprint')), $fingerprint + "`n", $utf8)
$zip = Join-Path $OutputDirectory $zipName
if ([IO.File]::Exists($zip)) { Remove-Item -LiteralPath $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::NoCompression, $true)
$zipLength = [uint64]([IO.FileInfo]$zip).Length
if ($zipLength -ge $limit) {
    Remove-Item -LiteralPath $zip -Force
    Remove-Item -LiteralPath (Join-Path $OutputDirectory ($folderName + '.fingerprint')) -Force -ErrorAction SilentlyContinue
    $reason = "$zipName is $zipLength bytes, at or above the $limit byte GitHub asset limit."
    if ($Publish -and $Variant -eq 'sensevoice') { throw $reason }
    if ($Publish) { Write-Skip $reason; exit 0 }
    throw $reason
}
Write-Output "$zip $zipLength $fingerprint"
