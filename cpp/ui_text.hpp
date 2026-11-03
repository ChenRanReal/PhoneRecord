#pragma once

#include "ui_resources.hpp"

/// Selected product language; technical log codes and protocol values stay invariant.
static std::atomic<int> g_uiLanguage{0};

/// Return the persisted ISO-style locale token for the current language.
static const char* uiLanguageCode() { const char* codes[] = {"zh-CN", "zh-TW", "en"}; return codes[std::max(0, std::min(2, g_uiLanguage.load()))]; }

/// Resolve only explicitly supported locales, reporting unsupported preferences to the caller.
static int uiLanguageIndex(const string& code) { return code == "zh-CN" ? 0 : code == "zh-TW" ? 1 : code == "en" ? 2 : -1; }

/// Resolve a complete locale entry; missing keys remain explicit in the selected language.
static const wchar_t* uiText(const char* key) {
    const int language = std::max(0, std::min(2, g_uiLanguage.load()));
    for (const auto& entry : kUiTexts) if (strcmp(key, entry.key) == 0) return entry.value[language];
    return language == 2 ? L"[Missing text]" : L"[缺少文案]";
}

/// Map stable diagnostic codes to actionable operator guidance in the existing locale.
static const wchar_t* diagnosticHint(const string& code) {
    if (code == "AUTO_RECORD_READY") return uiText("hint.automatic");
    if (code == "PHONE_SETUP_REQUIRED") return uiText("hint.phone_setup");
    if (code == "OFFLINE_MODE") return uiText("hint.offline");
    if (code == "WAV_ANALYSED") return uiText("hint.analysis");
    if (code == "WAV_CLOSED") return uiText("hint.finalized");
    if (code == "CTRL_TX") return uiText("hint.tx");
    if (code == "CTRL_RX") return uiText("hint.rx");
    if (code == "CTRL_CLOSE") return uiText("hint.close");
    if (code == "LOG_WRITE_FAILED" || code == "LOG_ROTATE_FAILED") return uiText("hint.log");
    if (code == "CONTROL_BIND_FAILED") return uiText("hint.control_bind");
    if (code == "EVENT_BIND_FAILED") return uiText("hint.event_bind");
    if (code == "CONTROL_NOT_OBSERVED") return uiText("hint.no_control");
    if (code == "CONTROL_NO_FRAME" || code == "CONTROL_NO_PAYLOAD" || code == "CONTROL_RESET" || code == "CONTROL_INCOMPLETE") return uiText("hint.no_frame");
    if (code == "UDP_PORT_EXHAUSTED") return uiText("hint.udp");
    if (code == "SESSION_LIMIT") return uiText("hint.sessions");
    if (code == "UNSUPPORTED_CODEC") return uiText("hint.codec");
    if (code == "CONTROL_REPLY_FAILED") return uiText("hint.reply");
    if (code == "RTP_NOT_RECEIVED" || code == "RTP_RECEIVE_ERROR") return uiText("hint.no_rtp");
    if (code == "CREDENTIAL_DECRYPT_FAILED") return uiText("hint.decrypt");
    if (code == "AUTH_SESSION_INVALID") return uiText("hint.cache");
    if (code.find("AUTH_") == 0 && code != "AUTH_VERIFIED" && code != "AUTH_CACHE_VALID") return uiText("hint.auth");
    if (code == "PHONE_BUSY") return uiText("hint.busy");
    if (code == "PHONE_UNREACHABLE" || code == "HTTP_TRANSPORT_FAILED" || code == "PHONE_STATE_UNAVAILABLE") return uiText("hint.unreachable");
    if (code == "WAV_OPEN_FAILED" || code == "WAV_WRITE_FAILED" || code == "WAV_FINALIZE_FAILED" || code == "RECORD_DIR_NOT_WRITABLE") return uiText("hint.directory");
    if (code == "DISK_SPACE_LOW") return uiText("hint.disk");
    if (code == "LOCAL_IP_NOT_ASSIGNED") return uiText("hint.ip");
    if (code == "AUDIO_SUSPECT") return uiText("hint.quality");
    if (code == "RECORDING_SAVED") return uiText("hint.saved");
    if (code == "CONFIG_INVALID" || code == "CONFIG_MISSING") return uiText("hint.config");
    if (code.find("CALL_ENDED_") == 0) return uiText("hint.ended");
    if (code == "STALE_STATE_RESET_FAILED") return uiText("hint.stale");
    if (code == "RECORDING" || code == "WAV_OPENED") return uiText("hint.recording");
    if (g_cfg.hookPort == 0) return uiText("hint.external");
    if (code == "AUTH_VERIFIED" || code == "AUTH_CACHE_VALID" || code == "LISTENING") return uiText("hint.ready");
    return uiText("hint.default");
}
