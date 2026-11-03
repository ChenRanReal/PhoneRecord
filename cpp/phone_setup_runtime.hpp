#pragma once

/// Hold local addresses/ports while setup runs; no call events or media are triggered.
class SetupPortReservation {
public:
    /// Close every reserved socket on all setup exit paths.
    ~SetupPortReservation() { for (SOCKET value : sockets_) closesocket(value); }
    /// Reserve a specific IPv4 and port using exclusive Windows binding.
    bool bindPort(int kind, int port, const string& ip) {
        SOCKET value = socket(AF_INET, kind, 0); if (value == INVALID_SOCKET) return false;
        const BOOL exclusive = TRUE; setsockopt(value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<u_short>(port)); InetPtonA(AF_INET, ip.c_str(), &address.sin_addr);
        if (bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(value); return false; }
        sockets_.push_back(value); return true;
    }
private:
    std::vector<SOCKET> sockets_;
};

/// Write a localized setup outcome without passwords, cookies or prior form values.
static bool writePhoneSetupReport(const wstring& path, const PhoneSetupResult& result) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "{\"version\":" << jsonQuoted(kProductVersion) << ",\"ok\":" << (result.ok ? "true" : "false")
        << ",\"code\":" << jsonQuoted(result.code) << ",\"message\":" << jsonQuoted(wideToUtf8(uiText(("setup." + result.code).c_str())))
        << ",\"target\":" << jsonQuoted(g_cfg.phoneIp + ":" + std::to_string(g_cfg.phonePort))
        << ",\"cause\":" << jsonQuoted(result.cause) << ",\"cause_message\":" << jsonQuoted(result.cause.empty() ? "" : wideToUtf8(uiText(("setup." + result.cause).c_str())))
        << ",\"plan_id\":" << jsonQuoted(result.planId) << ",\"backup\":" << jsonQuoted(result.backup)
        << ",\"applied\":" << (result.applied ? "true" : "false") << ",\"rollback_attempted\":" << (result.rollbackAttempted ? "true" : "false")
        << ",\"rollback_verified\":" << (result.rollbackVerified ? "true" : "false") << ",\"real_call_verified\":false,\"changes\":[";
    for (size_t index = 0; index < result.changes.size(); ++index) { if (index) output << ','; output << jsonQuoted(result.changes[index]); }
    output << "]}\n"; output.flush(); return static_cast<bool>(output);
}

/// Encode Unicode script data so the generated CMD stays ASCII on every Windows locale.
static string setupBase64(const wstring& value) {
    DWORD length = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size() * sizeof(wchar_t)), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &length)) return "";
    string output(length, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size() * sizeof(wchar_t)), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &output[0], &length)) return "";
    if (!output.empty() && !output.back()) output.pop_back(); return output;
}

/// Escape a translated fixed string for a PowerShell single-quoted literal.
static wstring setupPsText(const wchar_t* value) {
    wstring output; for (wchar_t byte : wstring(value)) { output += byte; if (byte == L'\'') output += byte; } return output;
}

/// Generate an operator-run script: preview, explicit APPLY, verified setup, then reopen.
static bool writePhoneSetupScript(const wstring& path) {
    wchar_t executable[32768]{}; GetModuleFileNameW(NULL, executable, static_cast<DWORD>(std::size(executable)));
    const wstring script =
        L"$ErrorActionPreference='Stop'\r\n"
        L"if($PSVersionTable.PSVersion.Major -lt 5){throw 'Windows PowerShell 5 or later is required'}\r\n"
        L"$exe=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + utf8ToWide(setupBase64(executable)) + L"'))\r\n"
        L"$cfg=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + utf8ToWide(setupBase64(g_configPath)) + L"'))\r\n"
        L"$report=Join-Path $env:TEMP ('PhoneRecord-setup-'+[guid]::NewGuid().ToString('N')+'.json')\r\n"
        L"function Invoke-Setup([string]$mode,[string]$plan=''){\r\n"
        L"  $setupArgs='--config '+[char]34+$cfg+[char]34+' '+$mode+' --setup-report '+[char]34+$report+[char]34\r\n"
        L"  if($plan){$setupArgs+=' --setup-plan '+$plan}\r\n"
        L"  if(Test-Path -LiteralPath $report){Remove-Item -LiteralPath $report}\r\n"
        L"  $p=Start-Process -FilePath $exe -ArgumentList $setupArgs -Wait -PassThru -WindowStyle Hidden\r\n"
        L"  if(!(Test-Path -LiteralPath $report)){throw ('Setup exited '+$p.ExitCode)}\r\n"
        L"  $r=Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json\r\n"
        L"  Write-Host ($r.code+': '+$r.message)\r\n"
        L"  if(!$r.ok){if($r.cause_message){Write-Host $r.cause_message};if($r.backup){Write-Host ('Backup: '+$r.backup)};throw $r.message}\r\n"
        L"  return $r\r\n}\r\n"
        L"try {\r\n"
        L"  Write-Host 'PhoneRecord - phone setup'\r\n"
        L"  Write-Host '" + setupPsText(uiText("setup.script_intro")) + L"'\r\n"
        L"  $r=Invoke-Setup '--phone-setup-preview'\r\n"
        L"  Write-Host ('Phone: '+$r.target)\r\n"
        L"  $r.changes | ForEach-Object {Write-Host ('  '+$_)}\r\n"
        L"  if(@($r.changes).Count -gt 0){\r\n"
        L"    $answer=Read-Host '" + setupPsText(uiText("setup.script_confirm")) + L"'\r\n"
        L"    if($answer -cne 'APPLY'){Write-Host 'Cancelled';exit 0}\r\n"
        L"    $r=Invoke-Setup '--phone-setup-apply' $r.plan_id\r\n"
        L"  }\r\n"
        L"  Write-Host '" + setupPsText(uiText("setup.script_done")) + L"'\r\n"
        L"  Start-Process -FilePath $exe -ArgumentList ('--config '+[char]34+$cfg+[char]34)\r\n"
        L"  Read-Host 'Enter' | Out-Null\r\n"
        L"} catch {Write-Host $_.Exception.Message -ForegroundColor Red;Read-Host 'Enter' | Out-Null;exit 1}\r\n";
    const string encoded = setupBase64(script); if (encoded.empty() || encoded.size() > 7800) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "@echo off\r\nsetlocal DisableDelayedExpansion\r\nrem Generated locally; no phone password is embedded.\r\npowershell.exe -NoProfile -EncodedCommand " << encoded << "\r\nexit /b %errorlevel%\r\n";
    output.flush(); return static_cast<bool>(output);
}

/// Run one exclusive, bounded setup transaction while the ordinary GUI is closed.
static int runPhoneSetupCommand(const string& action, const string& plan, const string& restore, const wstring& report) {
    PhoneSetupResult result;
    if (g_offline) result.code = "SETUP_OFFLINE";
    else if (!g_cfg.errors().empty() || g_cfg.hookPort == 0) result.code = "SETUP_CONFIG_INVALID";
    else {
        SetupPortReservation ports; bool udp = false;
        const bool local = ports.bindPort(SOCK_STREAM, 0, g_cfg.advertiseIp) && ports.bindPort(SOCK_STREAM, g_cfg.controlPort, "0.0.0.0") && ports.bindPort(SOCK_STREAM, g_cfg.hookPort, "0.0.0.0");
        if (local) for (int port = g_cfg.udpPortStart; port <= g_cfg.udpPortEnd && !udp; ++port) udp = ports.bindPort(SOCK_DGRAM, port, "0.0.0.0");
        if (!local || !udp) result.code = "SETUP_LOCAL_NOT_READY";
        else { Sleep(1200); if (!g_phone.login()) result.code = "SETUP_AUTH_FAILED"; else result = g_phone.configurePhone(action == "apply", plan, restore); }
    }
    if (!writePhoneSetupReport(report, result)) return 2;
    return result.ok ? 0 : 1;
}
