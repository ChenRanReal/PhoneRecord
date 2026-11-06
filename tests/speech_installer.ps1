<# .SYNOPSIS
Exercise production installer functions against a loopback fixture; never run the installation body.
#>
param([Parameter(Mandatory)][string]$Root,[Parameter(Mandatory)][string]$TestUrl)
$ErrorActionPreference='Stop'
if (-not $TestUrl.StartsWith('http://127.0.0.1:')) { throw 'Only a loopback fixture is allowed.' }
$ProgressFile=Join-Path $Root 'progress.json'
$utf8=[Text.UTF8Encoding]::new($false)
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '..\cpp\speech\install.ps1'),[ref]$null,[ref]$null)
foreach($function in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
    $source=$function.Extent.Text
    if($function.Name -eq 'Fetch') {
        # Relax only the extracted test function's transport guard for our owned HTTP fixture.
        # The shipped installer retains its HTTPS-only guard and has no test endpoint option.
        $source=$source.Replace('if ($response.ResponseUri.Scheme -ne ''https'') { throw ''network'' }','if ($response.ResponseUri.Host -ne ''127.0.0.1'') { throw ''fixture-host'' }')
    }
    Invoke-Expression $source
}
$script:Phase='runtime';$script:Resource='';$script:Origin='';$script:Operation='download';$script:NativeDetail='';$script:ExitCode=''
Publish 'runtime' 100
try { Fetch ($TestUrl+'/unavailable') (Join-Path $Root 'fixture.onnx') ('0'*64) 'SHA256' 'model'; throw 'Expected a server error.' }
catch { ReportFailure $_.Exception }
$errorState=Get-Content -LiteralPath $ProgressFile -Raw -Encoding UTF8 | ConvertFrom-Json
if($errorState.step -ne 'http_failed' -or $errorState.phase -ne 'model' -or $errorState.httpStatus -ne '503' -or $errorState.resource -ne 'fixture.onnx' -or $errorState.percent -ne '0') { throw 'HTTP failure facts were lost.' }
try { Fetch ($TestUrl+'/asset') (Join-Path $Root 'vad-fixture.onnx') ('0'*64) 'SHA256' 'vad'; throw 'Expected an integrity error.' }
catch { ReportFailure $_.Exception }
$errorState=Get-Content -LiteralPath $ProgressFile -Raw -Encoding UTF8 | ConvertFrom-Json
if($errorState.step -ne 'integrity' -or $errorState.phase -ne 'vad' -or [IO.File]::Exists((Join-Path $Root 'vad-fixture.onnx'))) { throw ('Invalid bytes were accepted: '+($errorState | ConvertTo-Json -Compress)) }
$script:Phase='runtime';$script:Operation='install';$script:NativeDetail='fixture dependency error';$script:ExitCode='37'
ReportFailure ([Exception]::new('runtime_failed'))
$errorState=Get-Content -LiteralPath $ProgressFile -Raw -Encoding UTF8 | ConvertFrom-Json
if($errorState.step -ne 'runtime_failed' -or $errorState.exitCode -ne '37' -or -not $errorState.detail.Contains('fixture dependency error')) { throw 'Native dependency diagnostics were discarded.' }
$script:Phase='model';$script:Operation='download';$script:NativeDetail='';$script:ExitCode=''
ReportFailure ([Security.Authentication.AuthenticationException]::new('fixture TLS handshake failure'))
$errorState=Get-Content -LiteralPath $ProgressFile -Raw -Encoding UTF8 | ConvertFrom-Json
if($errorState.step -ne 'network_tls' -or $errorState.phase -ne 'model') { throw 'TLS failure was misclassified.' }
$redacted=Redact ($Root+' https://fixture-user:fixture-secret@example.invalid/file?sample=fixture-private-query')
if($redacted.Contains($Root)-or$redacted.Contains('fixture-secret')-or$redacted.Contains('fixture-private-query')) { throw 'Diagnostic redaction failed.' }
Write-Output 'SPEECH_INSTALLER_OK http_status=1 phase=1 integrity=1 native_exit=1 tls=1 redaction=1'
