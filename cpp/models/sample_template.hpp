#pragma once

#include "../device_model_util.hpp"

/*
 * Template component with no phone protocol.
 * It demonstrates the export shape. It does not match a real device and is not the fallback.
 * Automatic selection ignores its model-text score because that score is below PR_DEVICE_MODEL_AUTO_MIN.
 */

/// Example established token. It is intentionally not a Fanvil token.
inline const char* const* sampleEstablishedTokens() {
    static const char* tokens[] = {"sample-est", nullptr};
    return tokens;
}

/// Example terminated token. It is intentionally not a Fanvil token.
inline const char* const* sampleTerminatedTokens() {
    static const char* tokens[] = {"sample-end", nullptr};
    return tokens;
}

/// Example setup token. It is intentionally not a Fanvil token.
inline const char* const* sampleReadyTokens() {
    static const char* tokens[] = {"sample-ready", nullptr};
    return tokens;
}

/// Example address token. It is intentionally not a Fanvil token.
inline const char* const* sampleAddressTokens() {
    static const char* tokens[] = {"sample-ip", nullptr};
    return tokens;
}

/// Empty profile used by the template. The shared web and recording executors stay disabled.
inline const PrDeviceProfile& sampleTemplateProfile() {
    static const PrDeviceProfile profile = [] {
        PrDeviceProfile value{};
        value.size = sizeof(value);
        value.id = "sample.template";
        value.display_name = "Sample template";
        value.capabilities = 0;
        value.priority = 0;
        value.fallback = 0;
        value.established_tokens = sampleEstablishedTokens();
        value.terminated_tokens = sampleTerminatedTokens();
        value.ready_tokens = sampleReadyTokens();
        value.address_tokens = sampleAddressTokens();
        value.event_path = "/event";
        value.event_query_key = "ev";
        value.event_query_alias = "event";
        value.auth_scheme = "none";
        value.record_page = "";
        value.action_page = "";
        value.start_path = "";
        value.stop_path = "";
        value.nonce_path = "";
        value.contacts_path = "";
        value.calllog_path = "";
        value.cookie_name = "";
        value.login_language = "";
        value.return_page = "";
        value.ready_state = 0;
        value.quiet_interval_ms = 0;
        value.media_wait_ms = 0;
        value.action_backoff_ms = 0;
        value.auth_backoff_ms = 0;
        value.ready_remember_ms = 0;
        value.login_page_markers = nullptr;
        value.protected_marker = "";
        value.record_status_regex = "";
        value.field_enable = "";
        value.field_type = "";
        value.field_codec = "";
        value.field_address = "";
        value.field_port = "";
        value.field_checkbox_manager = "";
        value.field_default_record = "";
        value.value_enabled = "";
        value.value_network = "";
        value.value_codec_pcma = "";
        value.value_codec_pcmu = "";
        value.field_established = "";
        value.field_terminated = "";
        value.field_setup_completed = "";
        value.field_ip_change = "";
        value.action_body_prefix = "";
        value.ctrl_start_request = "";
        value.ctrl_stop_request = "";
        value.ctrl_start_response = "";
        value.ctrl_stop_response = "";
        value.ctrl_codec_pcma = "";
        value.ctrl_codec_pcmu = "";
        value.ctrl_field_cmd = "";
        value.ctrl_field_id = "";
        value.ctrl_field_codec = "";
        value.ctrl_field_local = "";
        value.ctrl_field_remote = "";
        value.ctrl_field_direction = "";
        value.ctrl_field_name = "";
        value.directory_contact_query = "";
        value.directory_history_query = "";
        value.directory_name_field = "";
        value.directory_contact_numbers = nullptr;
        value.directory_history_numbers = nullptr;
        value.directory_roots = nullptr;
        return value;
    }();
    return profile;
}

/// Match only an explicit id or the template's own model text. The model-text score cannot win automatically.
inline int sampleTemplateMatch(const PrMatchInput* input, PrMatchResult* result) {
    if (!result) return 0;
    result->size = sizeof(*result);
    result->score = 0;
    result->reason = "none";
    if (!input) return 0;
    if (prEqualAscii(prText(input->user_override), "sample.template")) {
        result->score = 1000; result->reason = "user_override"; return 1;
    }
    if (prEqualAscii(prText(input->model_text), "SAMPLE-TEMPLATE")) {
        result->score = 100; result->reason = "model_text"; return 1;
    }
    return 0;
}

/// Classify the template tokens. Fanvil tokens stay unknown for this component.
inline int sampleTemplateClassify(const char* token) { return prClassifyToken(sampleTemplateProfile(), token); }

/// Built-in template component.
inline const PrDeviceModelV1* sampleTemplateModel() {
    static const PrDeviceModelV1 model{PR_DEVICE_MODEL_ABI, sizeof(PrDeviceModelV1), &sampleTemplateProfile(), sampleTemplateMatch, sampleTemplateClassify};
    return &model;
}
