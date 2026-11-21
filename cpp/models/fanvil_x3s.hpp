#pragma once

#include "../device_model_util.hpp"

/// Historical Fanvil X3S call-established callback tokens. The first value is written to the phone.
inline const char* const* fanvilEstablishedTokens() {
    static const char* tokens[] = {"est", "established", nullptr};
    return tokens;
}

/// Historical Fanvil X3S call-terminated callback tokens. The first value is written to the phone.
inline const char* const* fanvilTerminatedTokens() {
    static const char* tokens[] = {"end", "terminated", nullptr};
    return tokens;
}

/// Historical Fanvil startup callback token.
inline const char* const* fanvilReadyTokens() {
    static const char* tokens[] = {"ready", nullptr};
    return tokens;
}

/// Historical Fanvil IP-change callback token.
inline const char* const* fanvilAddressTokens() {
    static const char* tokens[] = {"ip", nullptr};
    return tokens;
}

/// Strings that distinguish the Fanvil web login page from a recording page.
inline const char* const* fanvilLoginMarkers() {
    static const char* tokens[] = {"logon_content", "Password Error", "Not Authorized", nullptr};
    return tokens;
}

/// Contact number elements read from contacts.xml.
inline const char* const* fanvilContactNumbers() {
    static const char* tokens[] = {"tel1", "tel2", "tel3", nullptr};
    return tokens;
}

/// History number element read from calllog.xml.
inline const char* const* fanvilHistoryNumbers() {
    static const char* tokens[] = {"num", nullptr};
    return tokens;
}

/// Fanvil directory roots that are valid when they contain no rows.
inline const char* const* fanvilDirectoryRoots() {
    static const char* tokens[] = {"contacts", "phonebook", "calllog", "records", "PhoneBook", "CallLog", nullptr};
    return tokens;
}

/// Fill the verified Fanvil X3S contract. Values match the previous in-tree literals.
inline const PrDeviceProfile& fanvilX3sProfile() {
    static const PrDeviceProfile profile = [] {
        PrDeviceProfile value{};
        value.size = sizeof(value);
        value.id = "fanvil.x3s";
        value.display_name = "Fanvil X3S";
        value.capabilities = PR_CAP_EVENT_INGESTION | PR_CAP_DISCOVERY | PR_CAP_CALL_METADATA | PR_CAP_WEB_API |
            PR_CAP_AUTO_SETUP | PR_CAP_DIRECTORY | PR_CAP_NETWORK_RECORDING;
        value.priority = 100;
        value.fallback = 1;
        value.established_tokens = fanvilEstablishedTokens();
        value.terminated_tokens = fanvilTerminatedTokens();
        value.ready_tokens = fanvilReadyTokens();
        value.address_tokens = fanvilAddressTokens();
        value.event_path = "/event";
        value.event_query_key = "ev";
        value.event_query_alias = "event";
        value.auth_scheme = "fanvil-nonce-md5";
        value.record_page = "/record.htm";
        value.action_page = "/action.htm";
        value.start_path = "/Start";
        value.stop_path = "/Stop";
        value.nonce_path = "/key==nonce";
        value.contacts_path = "/contacts.xml";
        value.calllog_path = "/calllog.xml";
        value.cookie_name = "auth";
        value.login_language = "cn";
        value.return_page = "/";
        value.ready_state = 4;
        value.quiet_interval_ms = 1100;
        value.media_wait_ms = 8000;
        value.action_backoff_ms = 1500;
        value.auth_backoff_ms = 300;
        value.ready_remember_ms = 65000;
        value.login_page_markers = fanvilLoginMarkers();
        value.protected_marker = "recordStatus";
        value.record_status_regex = "recordStatus\\s*=\\s*\"\\s*(\\d+)";
        value.field_enable = "EnableRecord_RW";
        value.field_type = "CSE_RecordType_RW";
        value.field_codec = "CSE_RecordVoiceCodec_R";
        value.field_address = "SYS_RecordAddr_RW";
        value.field_port = "SYS_RecordPort_RW";
        value.field_checkbox_manager = "CheckBoxManager";
        value.field_default_record = "DefaultRecord";
        value.value_enabled = "1";
        value.value_network = "1";
        value.value_codec_pcma = "PCMA";
        value.value_codec_pcmu = "PCMU";
        value.field_established = "AURL_CallEstablished_RW";
        value.field_terminated = "AURL_CallTerminated_RW";
        value.field_setup_completed = "AURL_SetupCompleted_RW";
        value.field_ip_change = "AURL_IpChange_RW";
        value.action_body_prefix = "DefaultRecord=";
        value.ctrl_start_request = "RecordStartRequest";
        value.ctrl_stop_request = "RecordStopRequest";
        value.ctrl_start_response = "RecordStartResponse";
        value.ctrl_stop_response = "RecordStopResponse";
        value.ctrl_codec_pcma = "PCMA";
        value.ctrl_codec_pcmu = "PCMU";
        value.ctrl_field_cmd = "cmd";
        value.ctrl_field_id = "id";
        value.ctrl_field_codec = "codec";
        value.ctrl_field_local = "local_number";
        value.ctrl_field_remote = "remote_number";
        value.ctrl_field_direction = "dir";
        value.ctrl_field_name = "remote_name";
        value.directory_contact_query = "//contact";
        value.directory_history_query = "//record";
        value.directory_name_field = "name";
        value.directory_contact_numbers = fanvilContactNumbers();
        value.directory_history_numbers = fanvilHistoryNumbers();
        value.directory_roots = fanvilDirectoryRoots();
        return value;
    }();
    return profile;
}

/// Claim Fanvil evidence. A bare vendor hit scores below an explicit X3S hit.
inline int fanvilX3sMatch(const PrMatchInput* input, PrMatchResult* result) {
    if (!result) return 0;
    result->size = sizeof(*result);
    result->score = 0;
    result->reason = "none";
    if (!input) return 0;
    const auto raise = [&](int score, const char* reason) {
        if (score > result->score) { result->score = score; result->reason = reason; }
    };
    if (prEqualAscii(prText(input->user_override), "fanvil.x3s")) raise(1000, "user_override");
    if (prContainsAscii(prText(input->model_text), "x3s")) raise(800, "model_text");
    if (prContainsAscii(prText(input->model_text), "fanvil")) raise(500, "vendor");
    if (prContainsExact(prText(input->http_body), "AURL_CallEstablished_RW")) raise(460, "action_url");
    if (prContainsExact(prText(input->http_body), "recordStatus") && prContainsExact(prText(input->http_body), "EnableRecord_RW"))
        raise(440, "record_page");
    if (prContainsAscii(prText(input->http_server), "fanvil")) raise(420, "server");
    return result->score > 0 ? 1 : 0;
}

/// Classify one Fanvil callback token, including the historical long aliases.
inline int fanvilX3sClassify(const char* token) { return prClassifyToken(fanvilX3sProfile(), token); }

/// Built-in Fanvil X3S component. The same object can be returned from a plugin export.
inline const PrDeviceModelV1* fanvilX3sModel() {
    static const PrDeviceModelV1 model{PR_DEVICE_MODEL_ABI, sizeof(PrDeviceModelV1), &fanvilX3sProfile(), fanvilX3sMatch, fanvilX3sClassify};
    return &model;
}
