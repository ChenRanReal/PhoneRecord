#include "../cpp/device_model.h"

#include <string.h>

/* Test-only phone used to prove an external library can outrank the built-in fallback. */

static PrDeviceProfile g_profile;
static PrDeviceModelV1 g_model;
static int g_ready = 0;

static const char* g_established[] = {"ring", 0};
static const char* g_terminated[] = {"drop", 0};

static void fill_profile(void) {
    if (g_ready) return;
    memset(&g_profile, 0, sizeof(g_profile));
    g_profile.size = (uint32_t)sizeof(g_profile);
    g_profile.id = "widget.phone";
    g_profile.display_name = "Widget phone";
    g_profile.capabilities = PR_CAP_EVENT_INGESTION | PR_CAP_DISCOVERY;
    g_profile.priority = 40;
    g_profile.fallback = 0;
    g_profile.established_tokens = g_established;
    g_profile.terminated_tokens = g_terminated;
    g_profile.ready_tokens = 0;
    g_profile.address_tokens = 0;
    g_profile.event_path = "/widget-event";
    g_profile.event_query_key = "ev";
    g_profile.event_query_alias = "";
    g_profile.auth_scheme = "none";
    g_profile.record_page = "";
    g_profile.action_page = "";
    g_profile.start_path = "";
    g_profile.stop_path = "";
    g_profile.nonce_path = "";
    g_profile.contacts_path = "";
    g_profile.calllog_path = "";
    g_profile.cookie_name = "";
    g_profile.login_language = "";
    g_profile.return_page = "";
    g_profile.protected_marker = "";
    g_profile.record_status_regex = "";
    g_profile.field_enable = "";
    g_profile.field_type = "";
    g_profile.field_codec = "";
    g_profile.field_address = "";
    g_profile.field_port = "";
    g_profile.field_checkbox_manager = "";
    g_profile.field_default_record = "";
    g_profile.value_enabled = "";
    g_profile.value_network = "";
    g_profile.value_codec_pcma = "";
    g_profile.value_codec_pcmu = "";
    g_profile.field_established = "";
    g_profile.field_terminated = "";
    g_profile.field_setup_completed = "";
    g_profile.field_ip_change = "";
    g_profile.action_body_prefix = "";
    g_profile.ctrl_start_request = "";
    g_profile.ctrl_stop_request = "";
    g_profile.ctrl_start_response = "";
    g_profile.ctrl_stop_response = "";
    g_profile.ctrl_codec_pcma = "";
    g_profile.ctrl_codec_pcmu = "";
    g_profile.ctrl_field_cmd = "";
    g_profile.ctrl_field_id = "";
    g_profile.ctrl_field_codec = "";
    g_profile.ctrl_field_local = "";
    g_profile.ctrl_field_remote = "";
    g_profile.ctrl_field_direction = "";
    g_profile.ctrl_field_name = "";
    g_profile.directory_contact_query = "";
    g_profile.directory_history_query = "";
    g_profile.directory_name_field = "";
    g_ready = 1;
}

static int widget_match(const PrMatchInput* input, PrMatchResult* result) {
    const char* model = input && input->model_text ? input->model_text : "";
    const char* body = input && input->http_body ? input->http_body : "";
    const char* override_id = input && input->user_override ? input->user_override : "";
    result->size = (uint32_t)sizeof(*result);
    result->score = 0;
    result->reason = "none";
    if (strcmp(override_id, "widget.phone") == 0) { result->score = 1000; result->reason = "user_override"; return 1; }
    if (strstr(model, "widget-9000")) { result->score = 880; result->reason = "model_text"; return 1; }
    if (strstr(body, "WIDGET-PHONE")) { result->score = 870; result->reason = "fingerprint"; return 1; }
    return 0;
}

static int widget_classify(const char* token) {
    if (!token) return PR_EVENT_UNKNOWN;
    if (strcmp(token, "ring") == 0) return PR_EVENT_ESTABLISHED;
    if (strcmp(token, "drop") == 0) return PR_EVENT_TERMINATED;
    return PR_EVENT_UNKNOWN;
}

PR_DEVICE_EXPORT const PrDeviceModelV1* pr_device_model_v1(void) {
    fill_profile();
    g_model.abi = PR_DEVICE_MODEL_ABI;
    g_model.size = (uint32_t)sizeof(g_model);
    g_model.profile = &g_profile;
    g_model.match = widget_match;
    g_model.classify_event = widget_classify;
    return &g_model;
}
