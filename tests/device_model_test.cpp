#include "../cpp/device_model_load.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

/// Stop at the first broken contract and name it without device data.
static void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}

/// Fresh registry with the two shipped components.
static void resetModels() {
    deviceModels().reset();
    deviceModels().ensure();
}

/// Fanvil literals that the recorder used before the component split.
static void checkFanvilContract() {
    const PrDeviceProfile& profile = fanvilX3sProfile();
    require(std::string(profile.id) == "fanvil.x3s", "fanvil id");
    require(profile.fallback == 1, "fanvil is the legacy fallback");
    require((profile.capabilities & PR_CAP_EVENT_INGESTION) != 0, "fanvil events");
    require((profile.capabilities & PR_CAP_WEB_API) != 0, "fanvil web");
    require((profile.capabilities & PR_CAP_AUTO_SETUP) != 0, "fanvil setup");
    require((profile.capabilities & PR_CAP_DIRECTORY) != 0, "fanvil directory");
    require((profile.capabilities & PR_CAP_NETWORK_RECORDING) != 0, "fanvil recording");
    require(std::string(profile.auth_scheme) == "fanvil-nonce-md5", "fanvil auth scheme");
    require(std::string(profile.record_page) == "/record.htm", "record page");
    require(std::string(profile.action_page) == "/action.htm", "action page");
    require(std::string(profile.start_path) == "/Start" && std::string(profile.stop_path) == "/Stop", "start stop paths");
    require(std::string(profile.nonce_path) == "/key==nonce", "nonce path");
    require(std::string(profile.contacts_path) == "/contacts.xml" && std::string(profile.calllog_path) == "/calllog.xml", "directory paths");
    require(std::string(profile.cookie_name) == "auth", "cookie name");
    require(std::string(profile.login_language) == "cn" && std::string(profile.return_page) == "/", "login form");
    require(profile.ready_state == 4, "ready state");
    require(profile.quiet_interval_ms == 1100, "quiet interval");
    require(profile.media_wait_ms == 8000, "media wait");
    require(profile.action_backoff_ms == 1500, "action backoff");
    require(profile.auth_backoff_ms == 300, "auth backoff");
    require(profile.ready_remember_ms == 65000, "ready memory");
    require(std::string(profile.record_status_regex) == "recordStatus\\s*=\\s*\"\\s*(\\d+)", "state regex");
    require(std::string(profile.field_established) == "AURL_CallEstablished_RW", "established field");
    require(std::string(profile.field_terminated) == "AURL_CallTerminated_RW", "terminated field");
    require(std::string(profile.field_setup_completed) == "AURL_SetupCompleted_RW", "setup field");
    require(std::string(profile.field_ip_change) == "AURL_IpChange_RW", "ip field");
    require(std::string(profile.field_enable) == "EnableRecord_RW", "enable field");
    require(std::string(profile.field_type) == "CSE_RecordType_RW", "type field");
    require(std::string(profile.field_codec) == "CSE_RecordVoiceCodec_R", "codec field");
    require(std::string(profile.field_address) == "SYS_RecordAddr_RW", "address field");
    require(std::string(profile.field_port) == "SYS_RecordPort_RW", "port field");
    require(std::string(profile.action_body_prefix) == "DefaultRecord=", "action body");
    require(std::string(profile.ctrl_start_request) == "RecordStartRequest", "start command");
    require(std::string(profile.ctrl_stop_request) == "RecordStopRequest", "stop command");
    require(std::string(profile.ctrl_field_remote) == "remote_number", "remote field");
    require(std::string(profile.ctrl_field_name) == "remote_name", "name field");
    require(std::string(profile.directory_contact_query) == "//contact", "contact query");
    require(std::string(profile.directory_history_query) == "//record", "history query");
    require(fanvilX3sClassify("est") == PR_EVENT_ESTABLISHED, "est");
    require(fanvilX3sClassify("Established") == PR_EVENT_ESTABLISHED, "established alias");
    require(fanvilX3sClassify("end") == PR_EVENT_TERMINATED, "end");
    require(fanvilX3sClassify("terminated") == PR_EVENT_TERMINATED, "terminated alias");
    require(fanvilX3sClassify("ready") == PR_EVENT_READY, "ready");
    require(fanvilX3sClassify("ip") == PR_EVENT_ADDRESS, "ip");
    require(fanvilX3sClassify("ring") == PR_EVENT_UNKNOWN, "fanvil rejects template token");
    require(prEventSame(profile, "est", "established") && !prEventSame(profile, "ready", "ip"), "alias pairs");
    require(deviceEventTarget(profile, "192.0.2.10", 18080, PR_EVENT_ESTABLISHED, "") == "http://192.0.2.10:18080/event?ev=est", "callback url");
    require(deviceEventTarget(profile, "192.0.2.10", 18080, PR_EVENT_TERMINATED, "ab") == "http://192.0.2.10:18080/event?ev=end&pair=ab", "paired callback");
    require(deviceControlStartReply(profile, "7", true, true, "192.0.2.20", 20000) ==
            "cmd=RecordStartResponse&id=7&result=success&reason=&server=192.0.2.20&port=20000\r\n", "start reply");
    require(deviceControlStartReply(profile, "7", false, false, "", 0) ==
            "cmd=RecordStartResponse&id=7&result=failed&reason=unsupported_codec&server=&port=0\r\n", "codec reply");
    require(deviceControlStopReply(profile, "7") == "cmd=RecordStopResponse&id=7&result=success\r\n", "stop reply");
    PrMatchInput input{}; input.size = sizeof(input);
    PrMatchResult result{};
    input.model_text = "Fanvil X3S";
    require(fanvilX3sMatch(&input, &result) == 1 && result.score == 800 && std::string(result.reason) == "model_text", "x3s score");
    input.model_text = "Fanvil X7";
    require(fanvilX3sMatch(&input, &result) == 1 && result.score == 500 && std::string(result.reason) == "vendor", "vendor score");
    input.model_text = "";
    input.http_body = "<input name=\"AURL_CallEstablished_RW\" value=\"\">";
    require(fanvilX3sMatch(&input, &result) == 1 && result.score == 460 && std::string(result.reason) == "action_url", "action fingerprint");
    input.http_body = "recordStatus=\"4\" EnableRecord_RW";
    require(fanvilX3sMatch(&input, &result) == 1 && result.score == 440 && std::string(result.reason) == "record_page", "record fingerprint");
    input.http_body = "login";
    input.http_server = "Fanvil";
    require(fanvilX3sMatch(&input, &result) == 1 && result.score == 420, "server fingerprint");
    input.http_server = "";
    require(fanvilX3sMatch(&input, &result) == 0 && result.score == 0, "empty evidence");
}

/// Selection order: override, strong evidence, then the Fanvil fallback.
static void checkSelection() {
    resetModels();
    DeviceModelSelection selected = deviceModels().select("", "", "", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "fallback" && selected.score == 0, "empty fallback");
    selected = deviceModels().select("", "SAMPLE-TEMPLATE", "", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "fallback", "weak template does not win");
    selected = deviceModels().select("sample.template", "", "", "", "");
    require(selected.id == "sample.template" && selected.reason == "user_override", "explicit template");
    require(activeClassifyEvent("sample-est") == PR_EVENT_ESTABLISHED, "template classify");
    require(activeClassifyEvent("est") == PR_EVENT_UNKNOWN, "template ignores fanvil token");
    selected = deviceModels().select("", "Fanvil X3S hardware", "", "", "");
    require(selected.id == "fanvil.x3s" && selected.score == 800 && selected.reason == "model_text", "model text selects fanvil");
    selected = deviceModels().select("", "", "recordStatus EnableRecord_RW", "", "");
    require(selected.id == "fanvil.x3s" && selected.score == 440 && selected.reason == "record_page", "page selects fanvil");
    selected = deviceModels().select("missing.model", "Fanvil X3S", "AURL_CallEstablished_RW", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "unknown_override" && selected.score == 0, "unknown override stays on fallback");
    const auto observed = deviceModels().observeHttpBody("WIDGET-PHONE");
    require(observed.id == "fanvil.x3s" && observed.reason == "unknown_override", "override stays pinned");
    selected = deviceModels().select("", "", "", "", "");
    deviceModels().observeHttpBody("<html>AURL_CallEstablished_RW</html>");
    selected = deviceModels().selection();
    require(selected.id == "fanvil.x3s" && selected.reason == "action_url", "observe upgrades fallback");
    deviceModels().observeHttpBody("login page");
    selected = deviceModels().selection();
    require(selected.id == "fanvil.x3s" && selected.reason == "action_url" && selected.score == 460, "weak page keeps the stronger match");
    require(deviceModelIdAllowed("fanvil.x3s") && !deviceModelIdAllowed("") && !deviceModelIdAllowed("fanvil x3s") && !deviceModelIdAllowed("bad/id"), "id charset");
}

/// Equal scores keep the higher priority. A greedy low score cannot replace the fallback.
static void checkPriority() {
    resetModels();
    static PrDeviceProfile low = fanvilX3sProfile();
    static PrDeviceProfile high = fanvilX3sProfile();
    low.id = "tie.low"; low.priority = 1; low.fallback = 0;
    high.id = "tie.high"; high.priority = 50; high.fallback = 0;
    const auto match = [](const PrMatchInput*, PrMatchResult* result) {
        result->size = sizeof(*result); result->score = 500; result->reason = "vendor"; return 1;
    };
    const auto classify = [](const char*) { return PR_EVENT_UNKNOWN; };
    static const PrDeviceModelV1 lowModel{PR_DEVICE_MODEL_ABI, sizeof(PrDeviceModelV1), &low, +match, +classify};
    static const PrDeviceModelV1 highModel{PR_DEVICE_MODEL_ABI, sizeof(PrDeviceModelV1), &high, +match, +classify};
    require(deviceModels().addBuiltIn(&lowModel), "add low");
    require(deviceModels().addBuiltIn(&highModel), "add high");
    const auto selected = deviceModels().select("", "anything", "", "", "");
    require(selected.id == "tie.high" && selected.score == 500, "higher priority wins a tie");
    static PrDeviceProfile weak = fanvilX3sProfile();
    weak.id = "tie.weak"; weak.priority = 1000; weak.fallback = 0;
    const auto weakMatch = [](const PrMatchInput*, PrMatchResult* result) {
        result->size = sizeof(*result); result->score = 100; result->reason = "model_text"; return 1;
    };
    static const PrDeviceModelV1 weakModel{PR_DEVICE_MODEL_ABI, sizeof(PrDeviceModelV1), &weak, +weakMatch, +classify};
    deviceModels().reset();
    deviceModels().ensure();
    require(deviceModels().addBuiltIn(&weakModel), "add weak");
    const auto ignored = deviceModels().select("", "", "", "", "");
    require(ignored.id == "fanvil.x3s" && ignored.reason == "fallback", "score below the automatic minimum stays on fallback");
}

/// Load a directory of plugins. The sample id is already built in, so its library is rejected.
static void checkPlugins(const std::string& directory) {
    deviceModels().reset();
    const int loaded = deviceModelsDiscover(directory);
    const auto ids = deviceModels().ids();
    bool widget = false, fanvil = false, sample = false, duplicate = false, opened = false;
    for (const auto& id : ids) {
        if (id == "widget.phone") widget = true;
        if (id == "fanvil.x3s") fanvil = true;
        if (id == "sample.template") sample = true;
    }
    for (const auto& failure : deviceModels().failures()) {
        if (failure.size() >= 11 && failure.compare(failure.size() - 11, 11, ": duplicate") == 0) duplicate = true;
        if (failure.size() >= 6 && failure.compare(failure.size() - 6, 6, ": open") == 0) opened = true;
    }
    require(loaded == 1 && widget && fanvil && sample, "widget loaded beside built-ins");
    require(duplicate && opened, "duplicate and unreadable plugins are skipped");
    auto selected = deviceModels().select("", "", "", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "fallback" && selected.loaded == 1, "plugin does not replace an empty match");
    selected = deviceModels().select("", "widget-9000", "", "", "");
    require(selected.id == "widget.phone" && selected.score == 880 && selected.reason == "model_text", "plugin model text wins");
    require(activeClassifyEvent("ring") == PR_EVENT_ESTABLISHED && activeClassifyEvent("drop") == PR_EVENT_TERMINATED, "plugin events");
    require(activeClassifyEvent("est") == PR_EVENT_UNKNOWN, "plugin does not inherit fanvil events");
    selected = deviceModels().select("", "", "recordStatus=\"4\" EnableRecord_RW", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "record_page", "fanvil page beats an unrelated plugin");
    selected = deviceModels().select("fanvil.x3s", "widget-9000", "WIDGET-PHONE", "", "");
    require(selected.id == "fanvil.x3s" && selected.reason == "user_override", "saved id beats a stronger plugin");
}

/// Write a file the loader must ignore without aborting discovery.
static void writeUnreadablePlugin(const std::string& directory) {
#if defined(_WIN32)
    const char* name = "bad.dll";
#else
    const char* name = "bad.so";
#endif
    const std::string path = directory + (directory.back() == '/' || directory.back() == '\\' ? "" : "/") + name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "not a plugin";
    require(static_cast<bool>(output), "write bad plugin");
}

/// Run the registry checks. Pass --plugins <directory> to load compiled libraries.
int main(int argc, char** argv) {
    try {
        checkFanvilContract();
        checkSelection();
        checkPriority();
        if (argc >= 3 && std::string(argv[1]) == "--plugins") {
            writeUnreadablePlugin(argv[2]);
            checkPlugins(argv[2]);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED " << error.what() << "\n";
        return 1;
    }
    std::cout << "PASSED device_model\n";
    return 0;
}
