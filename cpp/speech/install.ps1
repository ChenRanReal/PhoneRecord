<# .SYNOPSIS
Prepare a private, pinned CPU speech runtime. No machine installation, npm scripts or user audio.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$ProgressFile,
    [ValidateSet('huggingface.co','hf-mirror.com')][string]$Source = 'huggingface.co', [switch]$RuntimeOnly)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$utf8 = [Text.UTF8Encoding]::new($false)
$script:Model = 'sense'
$script:Phase = 'check'
$script:Resource = ''
$script:Origin = ''
$script:Operation = 'check'
$script:NativeDetail = ''
$script:ExitCode = ''

# Keep diagnostics useful without exposing signed URLs, credentials, user folders or proxy settings.
function Redact([string]$Value) {
    $value = [regex]::Replace($Value, 'https?://[^\s''"<>]+', {
        param($match)
        try { return ([Uri]$match.Value).GetLeftPart([UriPartial]::Authority) -replace '//[^/@]+@','//' } catch { return '[URL]' }
    })
    foreach ($private in @($env:USERPROFILE, $Root)) { if ($private) { $value = $value.Replace($private, '[local]') } }
    if ($value.Length -gt 3500) { $value = $value.Substring(0,3500) }
    return $value
}

function Publish([string]$Step, [int]$Percent = 0, [long]$Completed = 0, [long]$Total = 0) {
    # Publish only stable codes; never emit private paths or downloaded error pages.
    $body = @{step=$Step;percent=[string]$Percent;completed=[string]$Completed;total=[string]$Total;
        model=$script:Model;phase=$script:Phase;resource=$script:Resource;origin=$script:Origin;operation=$script:Operation} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($ProgressFile + '.pending', $body, $utf8)
    Move-Item -LiteralPath ($ProgressFile + '.pending') -Destination $ProgressFile -Force
}
# Compare cached bytes with the compiled release identity, never trusting a marker alone.
function Valid([string]$File, [string]$Hash, [string]$Algorithm, [long]$Size = 0) {
    if (-not [IO.File]::Exists($File)) { return $false }
    if ($Size -and ([IO.FileInfo]$File).Length -ne $Size) { return $false }
    $stream = $null; $digest = $null
    try {
        $stream = [IO.File]::OpenRead($File)
        $digest = if ($Algorithm -eq 'SHA512') { [Security.Cryptography.SHA512]::Create() } else { [Security.Cryptography.SHA256]::Create() }
        return ([BitConverter]::ToString($digest.ComputeHash($stream))).Replace('-','') -eq $Hash
    } finally { if ($stream) { $stream.Dispose() }; if ($digest) { $digest.Dispose() } }
}
# Stream a bounded HTTPS asset into a partial file, verify it, then replace the destination.
function Fetch([string]$Url, [string]$File, [string]$Hash, [string]$Algorithm, [string]$Step, [long]$Size = 0) {
    $script:Phase = $Step; $script:Resource = [IO.Path]::GetFileName($File)
    $script:Origin = ([Uri]$Url).Host; $script:Operation = 'download'
    $script:NativeDetail = ''; $script:ExitCode = ''
    Publish $Step 0
    if (Valid $File $Hash $Algorithm $Size) { Publish $Step 100; return }
    $request = [Net.HttpWebRequest]::Create($Url)
    $request.Timeout = 30000; $request.ReadWriteTimeout = 30000
    $response = $null; $stream = $null; $output = $null
    try {
        $response = $request.GetResponse()
        if ($response.ResponseUri.Scheme -ne 'https') { throw 'network' }
        $stream = $response.GetResponseStream()
        $output = [IO.File]::Open($File + '.part', [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $buffer = New-Object byte[] 65536
        [long]$received = 0; $last = [DateTime]::UtcNow; $started = $last
        while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $received += $count
            if (($Size -and $received -gt $Size) -or $received -gt 5000000000 -or ([DateTime]::UtcNow - $started).TotalHours -gt 1) { throw 'integrity' }
            $output.Write($buffer, 0, $count)
            if (([DateTime]::UtcNow - $last).TotalMilliseconds -ge 200) {
                $total = if ($Size) { $Size } else { $response.ContentLength }
                Publish $Step $(if ($total -gt 0) { [int][Math]::Min(99, $received * 100 / $total) } else { 0 }) $received $total
                $last = [DateTime]::UtcNow
            }
        }
        $output.Dispose(); $output = $null
        if (-not (Valid ($File + '.part') $Hash $Algorithm $Size)) { throw 'integrity' }
        Move-Item -LiteralPath ($File + '.part') -Destination $File -Force
        Publish $Step 100
    } finally {
        if ($output) { $output.Dispose() }; if ($stream) { $stream.Dispose() }; if ($response) { $response.Dispose() }
        if ([IO.File]::Exists($File + '.part')) { Remove-Item -LiteralPath ($File + '.part') -Force }
    }
}
# Decode npm's fixed SHA-512 integrity value without invoking a package lifecycle script.
function Sha512Hex([string]$Base64) { return ([BitConverter]::ToString([Convert]::FromBase64String($Base64))).Replace('-', '') }
# Persist one classified, redacted failure for both progress observers and the next application start.
function ReportFailure([Exception]$Cause) {
    $code = $Cause.Message
    $chain = @(); $exception = $Cause; $httpStatus = ''; $nativeCode = ''
    while ($exception) {
        $chain += ($exception.GetType().Name + ': ' + $exception.Message)
        $nativeCode = '0x' + $exception.HResult.ToString('X8')
        if ($exception -is [Net.WebException]) {
            $nativeCode = [string]$exception.Status
            if ($exception.Response -is [Net.HttpWebResponse]) { $httpStatus = [string][int]$exception.Response.StatusCode }
            switch ([string]$exception.Status) {
                'Timeout' { $code = 'network_timeout' }
                'NameResolutionFailure' { $code = 'network_dns' }
                'ProxyNameResolutionFailure' { $code = 'network_proxy' }
                'TrustFailure' { $code = 'network_tls' }
                'SecureChannelFailure' { $code = 'network_tls' }
                'ProtocolError' { $code = 'http_failed' }
            }
        }
        if ($exception -is [Security.Authentication.AuthenticationException]) { $code = 'network_tls' }
        if ($exception -is [UnauthorizedAccessException]) { $code = 'permission' }
        $exception = $exception.InnerException
    }
    if ($code -notin @('disk','integrity','runtime_failed','inference_failed','unsupported_os','permission','network_timeout','network_dns','network_proxy','network_tls','http_failed')) {
        $code = if ($script:Operation -eq 'download') { 'download_failed' } elseif ($script:Phase -eq 'runtime') { 'runtime_failed' } else { 'failed' }
    }
    $detail = Redact (($chain -join "`n") + $(if ($script:NativeDetail) { "`n" + $script:NativeDetail } else { '' }))
    $failure = @{step=$code;percent='0';completed='0';total='0';phase=$script:Phase;operation=$script:Operation;
        resource=$script:Resource;origin=$script:Origin;detail=$detail;httpStatus=$httpStatus;nativeCode=$nativeCode;exitCode=$script:ExitCode;
        model=$script:Model;timestamp=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $Root 'install-error.json'), $failure, $utf8)
    [IO.File]::WriteAllText($ProgressFile + '.pending', $failure, $utf8)
    Move-Item -LiteralPath ($ProgressFile + '.pending') -Destination $ProgressFile -Force
}
try {
    Publish 'check'
    if ([Version](Get-CimInstance Win32_OperatingSystem).Version -lt [Version]'10.0') { throw 'unsupported_os' }
    $Root = [IO.Path]::GetFullPath($Root)
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Root 'cache')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Root 'models')) | Out-Null
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root))
    if ($drive.AvailableFreeSpace -lt 1000000000) { throw 'disk' }
    $zip = Join-Path $Root 'cache\node-v24.11.0-win-x64.zip'
    Fetch 'https://nodejs.org/dist/v24.11.0/node-v24.11.0-win-x64.zip' $zip '1054540bce22b54ec7e50ebc078ec5d090700a77657607a58f6a64df21f49fdd' 'SHA256' 'runtime'
    $nodeRoot = Join-Path $Root 'node-v24.11.0-win-x64'
    $script:Operation = 'extract'; Publish 'runtime'
    # Re-extract the verified runtime on every explicit repair instead of trusting an existing executable.
    Expand-Archive -LiteralPath $zip -DestinationPath $Root -Force
    $node = Join-Path $nodeRoot 'node.exe'
    $npm = Join-Path $nodeRoot 'node_modules\npm\bin\npm-cli.js'
    $wrapper = Join-Path $Root 'cache\sherpa-onnx-node-1.13.8.tgz'
    $native = Join-Path $Root 'cache\sherpa-onnx-win-x64-1.13.8.tgz'
    Fetch 'https://registry.npmjs.org/sherpa-onnx-node/-/sherpa-onnx-node-1.13.8.tgz' $wrapper (Sha512Hex 'MsDMBdhLFTZ1GwvcGSSQhnS7g/EA8OMH6IYysCVUOM7j8Icty9KRc0E6YT1A5fWBsZwRfKOeh88QC95aRvS8ag==') 'SHA512' 'runtime'
    Fetch 'https://registry.npmjs.org/sherpa-onnx-win-x64/-/sherpa-onnx-win-x64-1.13.8.tgz' $native (Sha512Hex 'oZF1c9VPOKtMwn83Bboc5XSWL+76BRoyB3eUuVnCknBKxwSULZU2Foia9VHWzU+n4I12rPsP6z6H9Rp1hD9o8g==') 'SHA512' 'runtime'
    $runtime = Join-Path $Root 'runtime'
    [IO.Directory]::CreateDirectory($runtime) | Out-Null
    [IO.File]::WriteAllText((Join-Path $runtime 'package.json'), '{"name":"phonerecord-private-speech","version":"1.0.0","private":true}', $utf8)
    $script:Operation = 'install'; $script:Resource = 'sherpa-onnx-node 1.13.8'; $script:Origin = ''; Publish 'runtime'
    # Windows PowerShell converts native stderr to ErrorRecord. Capture it without mistaking warnings for a failed exit.
    $ErrorActionPreference = 'Continue'
    $LASTEXITCODE = $null
    $nativeOutput = & $node $npm install --prefix $runtime --cache (Join-Path $Root 'cache\npm') --offline --ignore-scripts --omit=optional --no-audit --no-fund $wrapper $native 2>&1 | Out-String
    $script:ExitCode = if ($null -eq $LASTEXITCODE) { 'not-started' } else { [string]$LASTEXITCODE }
    $ErrorActionPreference = 'Stop'
    $script:NativeDetail = Redact $nativeOutput
    if ($script:ExitCode -ne '0') { throw 'runtime_failed' }
    if ($RuntimeOnly) { Publish 'ready' 100; exit 0 }
    $asr = "https://$Source/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/2365baeacb507f821a0c8120fcee3d484dba7a07"
    Fetch "$asr/model.int8.onnx" (Join-Path $Root 'models\model.int8.onnx') 'c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51' 'SHA256' 'model' 239233841
    Fetch "$asr/tokens.txt" (Join-Path $Root 'models\tokens.txt') 'f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc' 'SHA256' 'model' 315894
    Fetch "https://$Source/csukuangfj/vad/resolve/fba88cd2e921609e7675c3aaf51e0b9b295da4bc/silero_vad.onnx" (Join-Path $Root 'models\silero_vad.onnx') 'a35ebf52fd3ce5f1469b2a36158dba761bc47b973ea3382b3186ca15b1f5af28' 'SHA256' 'vad' 1807522
    Publish 'verify'
    $script:Phase = 'load'; $script:Operation = 'load'; $script:Resource = 'SenseVoiceSmall (INT8)'; $script:Origin = ''; $script:NativeDetail = ''; $script:ExitCode = ''
    Publish 'load'
    $job = Join-Path $Root 'prepare.json'
    [IO.File]::WriteAllText($job, (@{root=$Root;language='auto';prepare=$true;progress=$ProgressFile} | ConvertTo-Json -Compress), $utf8)
    $ErrorActionPreference = 'Continue'
    $LASTEXITCODE = $null
    $nativeOutput = & $node (Join-Path $Root 'worker.cjs') $job 2>&1 | Out-String
    $script:ExitCode = if ($null -eq $LASTEXITCODE) { 'not-started' } else { [string]$LASTEXITCODE }
    $ErrorActionPreference = 'Stop'
    $script:NativeDetail = Redact $nativeOutput
    if ($script:ExitCode -ne '0') { throw 'inference_failed' }
    [IO.File]::WriteAllText((Join-Path $Root 'installed-v1'), 'sensevoice-int8-2024-07-17/sherpa-onnx-1.13.8/node-24.11.0', $utf8)
    Publish 'ready' 100
    if ([IO.File]::Exists((Join-Path $Root 'install-error.json'))) { Remove-Item -LiteralPath (Join-Path $Root 'install-error.json') -Force }
} catch {
    ReportFailure $_.Exception
    exit 1
}
