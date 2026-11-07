<# .SYNOPSIS
Official Windows CLI lifecycle and provider-specific text adapters. All requests are local JSON data.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Request)
$ErrorActionPreference='Stop'
[Console]::InputEncoding=[Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$utf8=[Text.UTF8Encoding]::new($false)
$job=Get-Content -LiteralPath $Request -Raw -Encoding UTF8 | ConvertFrom-Json
$provider=[string]$job.provider
$work=Split-Path -Parent $Request
$known=@{
 claude=@{command='claude.exe';url='https://claude.ai/install.ps1';folders=@("$env:USERPROFILE\.local\bin")}
 codex=@{command='codex.exe';url='https://chatgpt.com/codex/install.ps1';folders=@("$env:LOCALAPPDATA\Programs\OpenAI\Codex\bin","$env:USERPROFILE\.local\bin")}
 cursor=@{command='agent.ps1';url='https://cursor.com/install?win32=true';folders=@("$env:LOCALAPPDATA\cursor-agent")}
 antigravity=@{command='agy.exe';url='https://antigravity.google/cli/install.ps1';folders=@("$env:LOCALAPPDATA\agy\bin")}
 codebuddy=@{command='codebuddy.exe';url='https://www.codebuddy.cn/cli/install.ps1';folders=@("$env:LOCALAPPDATA\codebuddy\bin")}
 qwen=@{command='qwen.cmd';url='https://qwen-code-assets.oss-cn-hangzhou.aliyuncs.com/installation/install-qwen-standalone.ps1';folders=@("$env:LOCALAPPDATA\qwen-code\bin","$env:USERPROFILE\.qwen\bin")}
 kimi=@{command='kimi.exe';url='https://code.kimi.com/kimi-code/install.ps1';folders=@("$env:USERPROFILE\.kimi-code\bin","$env:USERPROFILE\.local\bin")}
 qoder=@{command='qoder.exe';url='https://qoder.com/install.ps1';folders=@("$env:USERPROFILE\.qoder\bin","$env:LOCALAPPDATA\qoder\bin","$env:USERPROFILE\.local\bin")}
 trae=@{command='traecli.exe';url='https://trae.cn/trae-cli/install_v2.ps1';folders=@("$env:USERPROFILE\.trae\bin","$env:LOCALAPPDATA\trae-cli","$env:USERPROFILE\.local\bin")}
}

function Write-Observation([string]$Step,[string]$Path='',[string[]]$Models=@(),[string]$Source='',[string]$Version='') {
    <# Publish normalized nonsecret facts only, never provider credentials or raw configuration. #>
    $data=@{step=$Step;path=$Path;models=($Models -join "`n");source=$Source;version=$Version}|ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $work 'observation.json'),$data,$utf8)
}

function Quote-Argument([string]$Value) {
    <# Quote one Windows CRT argument; prompts never become shell syntax. #>
    return '"'+[regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'
}

function Resolve-Tool {
    <# Search selected paths and official user locations, including freshly installed user PATH. #>
    if($job.executable) {
        if(Test-Path -LiteralPath $job.executable -PathType Leaf){return [IO.Path]::GetFullPath($job.executable)}
        throw 'cli_missing'
    }
    $definition=$known[$provider]
    $userPath=[Environment]::GetEnvironmentVariable('Path','User')
    if($userPath){$env:Path+=';'+$userPath}
    $command=Get-Command $definition.command -ErrorAction SilentlyContinue | Select-Object -First 1
    if($command){return $command.Source}
    foreach($folder in $definition.folders){$candidate=Join-Path $folder $definition.command;if(Test-Path -LiteralPath $candidate -PathType Leaf){return $candidate}}
    return ''
}

function Start-Cli([string]$Executable,[string[]]$Arguments,[bool]$Redirect=$true) {
    <# Native and trusted vendor PS1 entrypoints share argument-safe stdio, never cmd /c. #>
    $start=[Diagnostics.ProcessStartInfo]::new();$start.WorkingDirectory=$work;$start.UseShellExecute=$false
    if([IO.Path]::GetExtension($Executable) -eq '.ps1') {
        $Arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$Executable)+$Arguments
        $Executable=Join-Path $PSHOME 'powershell.exe'
        if(-not (Test-Path -LiteralPath $Executable)){ $Executable=Join-Path $PSHOME 'pwsh.exe' }
    }
    if([IO.Path]::GetExtension($Executable) -eq '.cmd' -and $provider -in @('qwen','qoder','trae')) {
        # Only the official Qwen entrypoint may use cmd; all shell metacharacters are rejected.
        foreach($part in (@($Executable)+$Arguments)){if($part -match '[%!&|<>^"\r\n]'){throw 'launch_failed'}}
        $start.FileName=Join-Path $env:SystemRoot 'System32\cmd.exe'
        $line='"'+$Executable+'" '+(($Arguments|ForEach-Object{'"'+$_+'"'}) -join ' ')
        $start.Arguments='/d /s /c "'+$line+'"'
    } else {
        if([IO.Path]::GetExtension($Executable) -ne '.exe'){throw 'cli_missing'}
        $start.FileName=$Executable;$start.Arguments=(($Arguments|ForEach-Object{Quote-Argument $_}) -join ' ')
    }
    $start.CreateNoWindow=$Redirect;$start.RedirectStandardInput=$Redirect;$start.RedirectStandardOutput=$Redirect;$start.RedirectStandardError=$Redirect
    if($Redirect){$start.StandardOutputEncoding=$utf8;$start.StandardErrorEncoding=$utf8}
    $child=[Diagnostics.Process]::new();$child.StartInfo=$start
    if(-not $child.Start()){throw 'launch_failed'}
    return $child
}

function Invoke-Cli([string]$Executable,[string[]]$Arguments,[string]$InputText='', [int]$Seconds=45) {
    <# Capture bounded management replies without allowing pipe backpressure or credential logging. #>
    $child=Start-Cli $Executable $Arguments
    try {
        $outBuffer=New-Object char[] 4096;$errBuffer=New-Object char[] 4096
        $output=[Text.StringBuilder]::new();$errors=[Text.StringBuilder]::new()
        $outRead=$child.StandardOutput.ReadAsync($outBuffer,0,4096);$errRead=$child.StandardError.ReadAsync($errBuffer,0,4096)
        $writing=$child.StandardInput.WriteAsync($InputText);$inputClosed=$false;$outDone=$false;$errDone=$false;$clock=[Diagnostics.Stopwatch]::StartNew()
        while(-not($outDone -and $errDone -and $child.HasExited)) {
            if($clock.Elapsed.TotalSeconds -gt $Seconds){try{$child.Kill()}catch{};throw 'timeout'}
            if(-not $inputClosed -and $writing.IsCompleted){$null=$writing.GetAwaiter().GetResult();$child.StandardInput.Close();$inputClosed=$true}
            if(-not $outDone -and $outRead.IsCompleted){$count=$outRead.GetAwaiter().GetResult();if($count -eq 0){$outDone=$true}else{$null=$output.Append($outBuffer,0,$count);if($output.Length -gt 2097152){throw 'output_limit'};$outRead=$child.StandardOutput.ReadAsync($outBuffer,0,4096)}}
            if(-not $errDone -and $errRead.IsCompleted){$count=$errRead.GetAwaiter().GetResult();if($count -eq 0){$errDone=$true}else{$null=$errors.Append($errBuffer,0,$count);if($errors.Length -gt 262144){throw 'output_limit'};$errRead=$child.StandardError.ReadAsync($errBuffer,0,4096)}}
            [Threading.Thread]::Sleep(10)
        }
        if($child.ExitCode -ne 0){throw 'process_failed'}
        return $output.ToString()
    } finally {try{if(-not $child.HasExited){$child.Kill()}}catch{};$child.Dispose()}
}

function Read-CodexModels([string]$Executable) {
    <# Use the documented app-server model/list protocol; no inference or token extraction. #>
    $child=Start-Cli $Executable @('app-server')
    try {
        $errors=$child.StandardError.ReadToEndAsync()
        $child.StandardInput.WriteLine('{"id":1,"method":"initialize","params":{"clientInfo":{"name":"phonerecord","version":"1.0"}}}')
        $requestId=1;$models=@();$pages=0
        while($true) {
            $task=$child.StandardOutput.ReadLineAsync();if(-not $task.Wait(30000)){throw 'timeout'}
            $line=$task.GetAwaiter().GetResult();if($null -eq $line){throw 'models_failed'}
            if($line.Length -gt 2097152){throw 'output_limit'}
            $reply=$line|ConvertFrom-Json
            if($reply.id -ne $requestId){continue}
            if($reply.error){throw 'models_failed'}
            if($requestId -eq 1) {
                $child.StandardInput.WriteLine('{"method":"initialized","params":{}}');$requestId=2
                $child.StandardInput.WriteLine('{"id":2,"method":"model/list","params":{"limit":100}}');continue
            }
            foreach($model in $reply.result.data){if($model.model){$models+=[string]$model.model}elseif($model.id){$models+=[string]$model.id}}
            if(-not $reply.result.nextCursor -or ++$pages -ge 5){break}
            $requestId++;$next=@{id=$requestId;method='model/list';params=@{limit=100;cursor=$reply.result.nextCursor}}|ConvertTo-Json -Compress -Depth 5
            $child.StandardInput.WriteLine($next)
        }
        return $models
    } finally {try{if(-not $child.HasExited){$child.Kill()}}catch{};$child.Dispose()}
}

try {
    if(-not $known.ContainsKey($provider)){throw 'cli_missing'}
    if($job.action -eq 'install' -and $job.executable -and -not(Test-Path -LiteralPath $job.executable -PathType Leaf)){$job.executable=''}
    $executable=Resolve-Tool
    if($job.action -eq 'install') {
        if(-not $executable) {
            # Only these reviewed official HTTPS endpoints may supply installer scripts.
            Write-Observation 'installing'
            [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
            $url=$known[$provider].url;$installer=Join-Path $work 'official-install.ps1'
            $download=Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 120
            $script=if($download.Content -is [byte[]]){$utf8.GetString($download.Content)}else{[string]$download.Content}
            if($download.StatusCode -ne 200 -or $script.Length -gt 2097152 -or $script -match '^\s*<!DOCTYPE'){throw 'install_failed'}
            [IO.File]::WriteAllText($installer,$script,$utf8)
            $hasher=[Security.Cryptography.SHA256]::Create()
            try {$hash=[BitConverter]::ToString($hasher.ComputeHash([IO.File]::ReadAllBytes($installer))).Replace('-','').ToLowerInvariant()}finally{$hasher.Dispose()}
            [IO.File]::WriteAllText((Join-Path $work 'installer.sha256'),$hash,$utf8)
            if($provider -eq 'codex'){$env:CODEX_NON_INTERACTIVE='1'}
            $shell=Join-Path $PSHOME 'powershell.exe';if(-not(Test-Path -LiteralPath $shell)){$shell=Join-Path $PSHOME 'pwsh.exe'}
            $null=Invoke-Cli $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',$installer) '' 900
            $executable=Resolve-Tool
        }
        if(-not $executable){throw 'install_failed'}
        $version=Invoke-Cli $executable @('--version')
        Write-Observation 'installed' $executable @() '' ($version.Trim());exit 0
    }
    if(-not $executable){Write-Observation 'not_installed';exit 0}
    if($job.action -eq 'login') {
        $arguments=@();if($provider -eq 'claude'){$arguments=@('auth','login')}elseif($provider -eq 'codex'){$arguments=@('login')}elseif($provider -eq 'cursor'){$arguments=@('login')}elseif($provider -eq 'kimi'){$arguments=@('login')}elseif($provider -eq 'trae'){$arguments=@('login')}
        $child=Start-Cli $executable $arguments $false;try{$child.WaitForExit();exit $child.ExitCode}finally{$child.Dispose()}
    }
    if($job.action -eq 'probe') {
        $version=Invoke-Cli $executable @('--version');Write-Observation 'installed' $executable @() '' ($version.Trim());exit 0
    }
    if($job.action -eq 'models') {
        $models=@();$source='cli_catalog'
        if($provider -eq 'cursor' -or $provider -eq 'antigravity') {
            $arguments=if($provider -eq 'cursor'){@('--list-models')}else{@('models')}
            $text=Invoke-Cli $executable $arguments
            # Official commands emit either slugs alone or slug followed by a human-readable description.
            foreach($line in ($text -split "`n")){$line=[regex]::Replace($line,'\x1B\[[0-9;]*[A-Za-z]','').Trim();if($line -match '^(?:[-*\u2022]\s*)?([a-z0-9][a-z0-9._:/-]+)(?:\s|$)' -and $Matches[1] -notin @('available','models','model','error','warning','usage')){$models+=$Matches[1]}}
            $source='cli_account'
        } elseif($provider -eq 'codex') {$models=@(Read-CodexModels $executable);$source='cli_catalog'}
        elseif($provider -eq 'codebuddy') {
            $help=Invoke-Cli $executable @('--help')
            if($help -match '(?s)Currently supported:\s*\(([^)]+)\)'){$models=@($Matches[1] -split ',\s*'|ForEach-Object{$_.Trim()})}
            $source='cli_catalog'
        } elseif($provider -eq 'claude') {$models=@('sonnet','opus','haiku','fable');$source='official_aliases'}
        elseif($provider -eq 'kimi') {
            $data=(Invoke-Cli $executable @('provider','list','--json'))|ConvertFrom-Json
            if($data.models){$models=@($data.models.PSObject.Properties.Name)};$source='local_config'
        } elseif($provider -eq 'qwen') {
            $settings=Join-Path $env:USERPROFILE '.qwen\settings.json'
            if(Test-Path -LiteralPath $settings){$data=Get-Content -LiteralPath $settings -Raw -Encoding UTF8|ConvertFrom-Json;foreach($entry in $data.modelProviders.PSObject.Properties){foreach($model in $entry.Value){if($model.id){$models+=[string]$model.id}}};if($data.model.name){$models+=[string]$data.model.name}}
            $source='local_config'
        } elseif($provider -eq 'qoder') {
            $text=Invoke-Cli $executable @('--list-models');$source='cli_account'
            foreach($line in ($text -split "`n")){$line=[regex]::Replace($line,'\x1B\[[0-9;]*[A-Za-z]','').Trim();if($line -match '^(?:[-*\u2022]\s*)?([A-Za-z0-9][A-Za-z0-9._:/-]+)' -and $Matches[1] -notin @('Model','Name','Available','models','model','error','warning','usage')){$models+=$Matches[1]}}
        } elseif($provider -eq 'trae') {
            $source='local_config'
            $traeFiles=@((Join-Path $env:USERPROFILE '.trae\traecli.toml'));if($env:APPDATA){$traeFiles=@(Join-Path $env:APPDATA 'trae_cli\trae_cli.yaml';Join-Path $env:APPDATA 'trae_cli\traecli.toml')+$traeFiles}
            foreach($file in $traeFiles){
                if(-not (Test-Path -LiteralPath $file)){continue}
                foreach($match in [regex]::Matches((Get-Content -LiteralPath $file -Raw -Encoding UTF8),'(?m)^\s*name\s*[:=]\s*[''"]?([A-Za-z0-9][A-Za-z0-9._:/-]+)')){if($match.Groups[1].Value -notin @('trae','traecli')){$models+=$match.Groups[1].Value}}
            }
        }
        $models=@($models|Where-Object{$_ -and $_.Length -le 256 -and $_ -match '^[A-Za-z0-9][A-Za-z0-9._:/\[\],= -]*$'}|Select-Object -Unique -First 200)
        if($source -in @('cli_account','cli_catalog') -and $models.Count -eq 0){throw 'models_failed'}
        Write-Observation 'models_ready' $executable $models $source;exit 0
    }
    if($job.action -eq 'run') {
        $arguments=@();$inputText=[IO.File]::ReadAllText([string]$job.input,$utf8)
        if($provider -eq 'cursor'){$arguments=@('-p','--mode','ask','--output-format','text')}
        elseif($provider -eq 'antigravity'){$arguments=@('-p','--mode','plan','--disable-slash-commands','--output-format','text')}
        elseif($provider -eq 'codex'){$arguments=@('exec','--ephemeral','--skip-git-repo-check','--sandbox','read-only','--output-last-message',(Join-Path $work 'final.txt'),'-')}
        elseif($provider -eq 'qwen'){$arguments=@('-p','Analyze the supplied conversation only.','--safe-mode','--max-tool-calls','0','--output-format','text')}
        elseif($provider -eq 'qoder'){$arguments=@('-p','--output-format','text')}
        elseif($provider -eq 'trae'){$arguments=@('exec','--sandbox','read-only','--ask-for-approval','never','--skip-git-repo-check','--ephemeral','-o',(Join-Path $work 'final.txt'))}
        elseif($provider -eq 'kimi'){
            # The official no-tools agent file avoids Windows command-line length limits.
            $agent=Join-Path $work 'analysis-agent.md'
            $header="---`nname: phonerecord-analysis`ndescription: Analyze supplied phone-call evidence only`ntools: []`nsubagents: []`n---`n"
            [IO.File]::WriteAllText($agent,$header+$inputText,$utf8)
            $arguments=@('--agent-file',$agent,'-p','Analyze the conversation evidence supplied in your instructions.','--output-format','text');$inputText=''
        }
        else {throw 'cli_missing'}
        if($job.model){$arguments+=@('--model',[string]$job.model)}
        if($job.effort -and $provider -eq 'codex'){$arguments+=@('-c',('model_reasoning_effort='+([string]$job.effort|ConvertTo-Json -Compress)))}
        if($job.effort -and $provider -eq 'antigravity'){$arguments+=@('--effort',[string]$job.effort)}
        if($job.effort -and $provider -eq 'qoder'){$arguments+=@('--reasoning-effort',[string]$job.effort)}
        $text=Invoke-Cli $executable $arguments $inputText ([int]$job.timeout)
        $final=Join-Path $work 'final.txt';if(($provider -eq 'codex' -or $provider -eq 'trae') -and (Test-Path -LiteralPath $final)){$text=[IO.File]::ReadAllText($final,$utf8)}
        [Console]::Write($text);exit 0
    }
    throw 'models_failed'
} catch {
    $code=$_.Exception.Message
    if($code -notin @('cli_missing','launch_failed','timeout','output_limit','process_failed','models_failed','install_failed','context_limit')){$code=if($job.action -eq 'install'){'install_failed'}else{'models_failed'}}
    Write-Observation $code
    [Console]::Error.WriteLine($code)
    exit 1
}
