#pragma once

/// One address-free capability result, safe to show in help or forward to another user.
struct CompatibilityItem {
    /// Stable resource suffix and outcome: pass, fail, pending or skip.
    string key, status, advice;
};

/// Read-only inspection result; web/config checks do not substitute for an audio test.
struct CompatibilityReport {
    /// idle, running, ready, settings, unsupported, unavailable, deferred or offline.
    string verdict = "idle", checkedAt;
    /// Call boundary lets only a later call contribute runtime recording evidence.
    uint64_t baselineAttempts = 0, revision = 0;
    /// The profile was recognized by its recording fields, not an inferred device model.
    bool recognizedProfile = false, externalTrigger = false;
    /// Ordered findings contain keys only, never actual IPs, URLs or credentials.
    std::vector<CompatibilityItem> items;
};

/// Guard the compatibility snapshot independently of GUI and phone transport locks.
static std::mutex g_compatibilityMutex;
static CompatibilityReport g_compatibilityReport;

/// Copy inspection evidence without borrowing mutable worker-owned storage.
static CompatibilityReport compatibilitySnapshot() {
    std::lock_guard<std::mutex> guard(g_compatibilityMutex); return g_compatibilityReport;
}

/// Publish one complete bounded result; technical logs exclude all inspected values.
static void publishCompatibility(CompatibilityReport report) {
    std::lock_guard<std::mutex> guard(g_compatibilityMutex);
    report.revision = g_compatibilityReport.revision + 1; g_compatibilityReport = std::move(report);
}

/// Start a fresh inspection, invalidating prior evidence without changing recording state.
static void beginCompatibility() {
    CompatibilityReport report; report.verdict = "running"; report.checkedAt = diagnosticTime();
    report.baselineAttempts = diagnosticSnapshot().attempts; report.externalTrigger = g_cfg.hookPort == 0;
    publishCompatibility(std::move(report));
}

/// Decode HTML attribute entities once; unrecognized sequences remain literal.
static string decodeHtmlAttribute(const string& value) {
    string output;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '&') { output += value[index]; continue; }
        const size_t end = value.find(';', index + 1);
        if (end == string::npos || end - index > 12) { output += '&'; continue; }
        const string entity = value.substr(index + 1, end - index - 1);
        char decoded = 0;
        if (entity == "amp") decoded = '&'; else if (entity == "quot") decoded = '"';
        else if (entity == "apos" || entity == "#39") decoded = '\'';
        else if (entity == "lt") decoded = '<'; else if (entity == "gt") decoded = '>';
        else if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            const string digits = entity.substr(hex ? 2 : 1);
            if (!digits.empty() && digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") == string::npos) {
                const unsigned long code = strtoul(digits.c_str(), NULL, hex ? 16 : 10);
                if (code > 0 && code < 128) decoded = static_cast<char>(code);
            }
        }
        if (!decoded) output += '&'; else { output += decoded; index = end; }
    }
    return output;
}

/// Parse bounded attributes while respecting quoted spaces and greater-than signs.
static std::map<string, string> htmlAttributes(const string& tag) {
    std::map<string, string> attributes;
    size_t position = tag.find_first_of(" \t\r\n");
    if (position == string::npos) return attributes;
    while (position < tag.size() && attributes.size() < 128) {
        while (position < tag.size() && isspace(static_cast<unsigned char>(tag[position]))) ++position;
        if (position == tag.size() || tag[position] == '>' || tag[position] == '/') break;
        const size_t start = position;
        while (position < tag.size() && !isspace(static_cast<unsigned char>(tag[position])) && tag[position] != '=' && tag[position] != '>') ++position;
        const string key = toLower(tag.substr(start, position - start));
        if (key.empty()) break;
        while (position < tag.size() && isspace(static_cast<unsigned char>(tag[position]))) ++position;
        string value;
        if (position < tag.size() && tag[position] == '=') {
            ++position; while (position < tag.size() && isspace(static_cast<unsigned char>(tag[position]))) ++position;
            if (position < tag.size() && (tag[position] == '\'' || tag[position] == '"')) {
                const char quote = tag[position++]; const size_t first = position;
                while (position < tag.size() && tag[position] != quote) ++position;
                value = tag.substr(first, position - first); if (position < tag.size()) ++position;
            } else {
                const size_t first = position;
                while (position < tag.size() && !isspace(static_cast<unsigned char>(tag[position])) && tag[position] != '>') ++position;
                value = tag.substr(first, position - first);
            }
        }
        attributes[key] = trim(decodeHtmlAttribute(value));
    }
    return attributes;
}

/// Find one real tag boundary without terminating inside a quoted attribute value.
static size_t htmlTagEnd(const string& body, size_t start) {
    char quote = 0;
    for (size_t index = start; index < body.size() && index - start < 16384; ++index) {
        const char byte = body[index];
        if (quote) { if (byte == quote) quote = 0; }
        else if (byte == '\'' || byte == '"') quote = byte;
        else if (byte == '>') return index;
    }
    return string::npos;
}

/// Read the selected logical recording fields used by the current supported web profile.
static std::map<string, string> readPhoneForm(const string& body) {
    std::map<string, string> fields;
    if (body.size() > 2 * 1024 * 1024) return fields;
    const string lower = toLower(body); size_t position = 0;
    for (int tags = 0; tags < 1024 && (position = lower.find('<', position)) != string::npos; ++tags) {
        if (lower.compare(position, 4, "<!--") == 0) { const size_t end = lower.find("-->", position + 4); if (end == string::npos) break; position = end + 3; continue; }
        if (lower.compare(position, 7, "<script") == 0 || lower.compare(position, 6, "<style") == 0) {
            const size_t end = lower.find(lower.compare(position, 7, "<script") == 0 ? "</script" : "</style", position + 1);
            if (end == string::npos) break; position = htmlTagEnd(body, end); if (position == string::npos) break; ++position; continue;
        }
        const bool input = lower.compare(position, 6, "<input") == 0 && position + 6 < lower.size() && isspace(static_cast<unsigned char>(lower[position + 6]));
        const bool select = lower.compare(position, 7, "<select") == 0 && position + 7 < lower.size() && isspace(static_cast<unsigned char>(lower[position + 7]));
        if (!input && !select) { ++position; continue; }
        const size_t end = htmlTagEnd(body, position); if (end == string::npos) break;
        auto attributes = htmlAttributes(body.substr(position, end - position + 1)); position = end + 1;
        const string name = attributes["name"];
        if (name.empty()) continue;
        if (input) {
            const string type = toLower(attributes["type"]);
            if (type == "checkbox") fields[name] = attributes.count("checked") ? "1" : "0";
            else if (type != "radio" || attributes.count("checked")) fields[name] = attributes["value"];
        } else {
            const size_t close = lower.find("</select", position); if (close == string::npos) break;
            size_t option = position; bool chosen = false, found = false; string value;
            for (int count = 0; count < 128 && (option = lower.find("<option", option)) != string::npos && option < close; ++count) {
                const size_t last = htmlTagEnd(body, option); if (last == string::npos || last > close) break;
                const auto item = htmlAttributes(body.substr(option, last - option + 1)); option = last + 1;
                const auto selected = item.find("selected"), content = item.find("value");
                if (content != item.end() && (!found || selected != item.end())) { value = content->second; found = true; }
                if (selected != item.end()) { chosen = true; break; }
            }
            if (found || chosen) fields[name] = value;
            position = close + 8;
        }
    }
    return fields;
}

/// Compare callback destination and event privately; no inspected address enters the result.
static bool compatibleCallback(const string& value, const char* event) {
    const string url = trim(value); const size_t scheme = url.find("://");
    if (scheme == string::npos || toLower(url.substr(0, scheme)) != "http") return false;
    const size_t path = url.find('/', scheme + 3); if (path == string::npos) return false;
    const string authority = url.substr(scheme + 3, path - scheme - 3);
    const string expected = g_cfg.advertiseIp + ":" + std::to_string(g_cfg.hookPort);
    if (authority != expected && !(g_cfg.hookPort == 80 && authority == g_cfg.advertiseIp)) return false;
    const size_t question = url.find('?', path); if (question == string::npos || url.substr(path, question - path) != "/event") return false;
    const size_t fragment = url.find('#', question);
    const string query = url.substr(question + 1, fragment == string::npos ? string::npos : fragment - question - 1);
    size_t first = 0;
    while (first <= query.size()) {
        const size_t end = query.find('&', first); const string item = query.substr(first, end == string::npos ? string::npos : end - first);
        const size_t equal = item.find('=');
        if (equal != string::npos && (item.substr(0, equal) == "ev" || item.substr(0, equal) == "event")) {
            const string type = toLower(item.substr(equal + 1));
            return type == event || (string(event) == "est" && type == "established") || (string(event) == "end" && type == "terminated");
        }
        if (end == string::npos) break; first = end + 1;
    }
    return false;
}

/// Promote only a later completed phone-origin call; synthetic or earlier audio cannot satisfy it.
static bool compatibilityRuntimeVerified(const CompatibilityReport& report, const DiagnosticSnapshot& state, long sessions) {
    return report.verdict == "ready" && state.attempts > report.baselineAttempts && !state.callActive && sessions == 0 &&
        state.attemptTcp && state.attemptReply && state.attemptRtp && state.attemptWav && state.quality == "audio" &&
        state.detail.find("source=configured_phone") != string::npos &&
        (report.externalTrigger || (state.triggerSource == "phone_event" && state.attemptHttp));
}

/// Serialize an address-free inspection for local automation and support readback.
static string compatibilityJson() {
    const auto report = compatibilitySnapshot();
    const bool runtime = compatibilityRuntimeVerified(report, diagnosticSnapshot(), g_sessionCount.load());
    std::ostringstream output;
    output << "{\"revision\":" << report.revision << ",\"verdict\":" << jsonQuoted(runtime ? "runtime_verified" : report.verdict)
           << ",\"checked_at\":" << jsonQuoted(report.checkedAt) << ",\"recognized_profile\":" << (report.recognizedProfile ? "true" : "false")
           << ",\"runtime_verified\":" << (runtime ? "true" : "false") << ",\"addresses_hidden\":true,\"checks\":[";
    for (size_t index = 0; index < report.items.size(); ++index) {
        if (index) output << ',';
        const auto& item = report.items[index]; const bool media = runtime && item.key == "runtime";
        output << "{\"key\":" << jsonQuoted(item.key) << ",\"status\":" << jsonQuoted(media ? "pass" : item.status)
               << ",\"advice\":" << jsonQuoted(media ? "runtime_ok" : item.advice) << '}';
    }
    return output.str() + "]}";
}
