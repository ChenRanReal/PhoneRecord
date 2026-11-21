#ifndef PHONERECORD_DEVICE_MODEL_H
#define PHONERECORD_DEVICE_MODEL_H

#include <stdint.h>

/* Stable ABI for built-in device models and libraries in the plugins directory. Bump when fields move. */
#define PR_DEVICE_MODEL_ABI 1

/* Automatic selection ignores weaker scores so a template cannot replace Fanvil X3S. */
#define PR_DEVICE_MODEL_AUTO_MIN 400

#define PR_CAP_EVENT_INGESTION 0x0001u
#define PR_CAP_DISCOVERY 0x0002u
#define PR_CAP_CALL_METADATA 0x0004u
#define PR_CAP_WEB_API 0x0008u
#define PR_CAP_AUTO_SETUP 0x0010u
#define PR_CAP_DIRECTORY 0x0020u
#define PR_CAP_NETWORK_RECORDING 0x0040u

#define PR_EVENT_UNKNOWN 0
#define PR_EVENT_ESTABLISHED 1
#define PR_EVENT_TERMINATED 2
#define PR_EVENT_READY 3
#define PR_EVENT_ADDRESS 4

#if defined(_WIN32)
#define PR_DEVICE_EXPORT __declspec(dllexport)
#else
#define PR_DEVICE_EXPORT __attribute__((visibility("default")))
#endif

/* Evidence the host already has. Unused pointers are null; strings are UTF-8. */
typedef struct PrMatchInput {
    /* Byte size of this struct, so a plugin can ignore fields it does not know. */
    uint32_t size;
    /* config.ini device_model, or empty when the operator left matching automatic. */
    const char* user_override;
    /* Model text from a phone page or a future discovery source. Empty when unknown. */
    const char* model_text;
    /* One HTTP body used as a fingerprint. Empty when the phone has not been read. */
    const char* http_body;
    /* HTTP Server header. Empty when the response did not include one. */
    const char* http_server;
    /* Normalized Ethernet identity. Empty when DHCP pairing is not in use. */
    const char* mac;
} PrMatchInput;

/* Positive scores compete. Zero means this component does not claim the evidence. */
typedef struct PrMatchResult {
    /* Byte size of this struct. */
    uint32_t size;
    /* Higher wins. Automatic selection requires at least PR_DEVICE_MODEL_AUTO_MIN. */
    int score;
    /* Stable literal such as model_text, vendor, action_url, record_page or server. */
    const char* reason;
} PrMatchResult;

/*
 * Phone-specific contract consumed by the shared recorder.
 * Scalar strings are never null after registration. Lists are NULL-terminated and may be null.
 * The first token of each event list is the value written into a new callback URL.
 */
typedef struct PrDeviceProfile {
    /* Byte size of this struct. The host rejects a different size. */
    uint32_t size;
    /* Stable id, ASCII letters, digits, dot, underscore or dash, at most 64 characters. */
    const char* id;
    /* Operator-facing name. It is not a match key. */
    const char* display_name;
    /* Bitwise OR of PR_CAP_* values this component implements. */
    uint32_t capabilities;
    /* Tie break when two automatic scores are equal. Larger wins. */
    int priority;
    /* Non-zero only for the legacy component used when nobody else matches. */
    int fallback;

    /* Accepted call-established callback tokens, canonical token first. */
    const char* const* established_tokens;
    /* Accepted call-terminated callback tokens, canonical token first. */
    const char* const* terminated_tokens;
    /* Accepted startup or setup-completed callback tokens, canonical token first. */
    const char* const* ready_tokens;
    /* Accepted IP-change callback tokens, canonical token first. */
    const char* const* address_tokens;
    /* Absolute callback path, including the leading slash. */
    const char* event_path;
    /* Primary query key for the event token. */
    const char* event_query_key;
    /* Accepted alias of event_query_key. Empty when there is no alias. */
    const char* event_query_alias;

    /* Host executor name. fanvil-nonce-md5 reuses the existing web client. none disables it. */
    const char* auth_scheme;
    /* Authenticated recording settings page. */
    const char* record_page;
    /* Authenticated action-URL page. */
    const char* action_page;
    /* Absolute path of the recording Start CGI. */
    const char* start_path;
    /* Absolute path of the recording Stop CGI. */
    const char* stop_path;
    /* Nonce path without the cache-busting query. */
    const char* nonce_path;
    /* Contact export path. */
    const char* contacts_path;
    /* Call-history export path. */
    const char* calllog_path;
    /* Session cookie name. */
    const char* cookie_name;
    /* Language field posted with the challenge. */
    const char* login_language;
    /* Return path posted with the challenge. */
    const char* return_page;
    /* Recording-page state that means idle and ready for Start. */
    int ready_state;
    /* Minimum milliseconds between HTTP exchanges to the phone. */
    int quiet_interval_ms;
    /* Milliseconds Start waits for the first local RTP packet. */
    int media_wait_ms;
    /* Milliseconds multiplied into a busy-action retry. */
    int action_backoff_ms;
    /* Milliseconds multiplied by the attempt number after a rejected login. */
    int auth_backoff_ms;
    /* How long a ready-state observation may be reused before Start. */
    int ready_remember_ms;
    /* Substrings that identify the phone's login page. NULL-terminated. */
    const char* const* login_page_markers;
    /* Substring required on an authenticated recording page. */
    const char* protected_marker;
    /* ECMAScript regex with one capture group for the recording state number. */
    const char* record_status_regex;

    /* Recording-form field names and the option values this host knows how to write. */
    const char* field_enable;
    const char* field_type;
    const char* field_codec;
    const char* field_address;
    const char* field_port;
    const char* field_checkbox_manager;
    const char* field_default_record;
    const char* value_enabled;
    const char* value_network;
    const char* value_codec_pcma;
    const char* value_codec_pcmu;
    const char* field_established;
    const char* field_terminated;
    const char* field_setup_completed;
    const char* field_ip_change;
    /* POST body prefix before the Start or Stop action name. */
    const char* action_body_prefix;

    /* TCP recording command names and the caller-id keys inside those commands. */
    const char* ctrl_start_request;
    const char* ctrl_stop_request;
    const char* ctrl_start_response;
    const char* ctrl_stop_response;
    const char* ctrl_codec_pcma;
    const char* ctrl_codec_pcmu;
    const char* ctrl_field_cmd;
    const char* ctrl_field_id;
    const char* ctrl_field_codec;
    const char* ctrl_field_local;
    const char* ctrl_field_remote;
    const char* ctrl_field_direction;
    const char* ctrl_field_name;

    /* XPath for one contact or one history row, and the element names under it. */
    const char* directory_contact_query;
    const char* directory_history_query;
    const char* directory_name_field;
    const char* const* directory_contact_numbers;
    const char* const* directory_history_numbers;
    /* Document element names that are valid even when they contain no rows. */
    const char* const* directory_roots;
} PrDeviceProfile;

/* Returns 1 when this component claims the input. A zero score is not a claim. */
typedef int (*PrDeviceMatchFn)(const PrMatchInput* input, PrMatchResult* result);
/* Returns a PR_EVENT_* kind. Unknown tokens return PR_EVENT_UNKNOWN. */
typedef int (*PrDeviceClassifyFn)(const char* token);

/* Exported from a plugin as pr_device_model_v1. Built-ins use the same layout. */
typedef struct PrDeviceModelV1 {
    /* Must equal PR_DEVICE_MODEL_ABI. */
    uint32_t abi;
    /* Byte size of this struct. */
    uint32_t size;
    /* Immutable profile. All scalar strings are non-null. */
    const PrDeviceProfile* profile;
    /* Match function. Required. */
    PrDeviceMatchFn match;
    /* Event classifier. Required. */
    PrDeviceClassifyFn classify_event;
} PrDeviceModelV1;

#endif
