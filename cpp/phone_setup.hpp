#pragma once

/// One complete supported form; checkbox wire values are retained separately.
struct SetupForm {
    std::map<string, string> values, checkboxes;
};

/// Operator-visible outcome; original form values are confined to encrypted backups.
struct PhoneSetupResult {
    string code = "SETUP_FAILED", planId, backup, cause;
    std::vector<string> changes;
    bool ok = false, applied = false, rollbackAttempted = false, rollbackVerified = false;
};

/// Owned coordinator snapshot; only completed previews may be confirmed by the GUI.
struct PhoneSetupProgress {
    uint64_t revision = 0;
    bool running = false, apply = false;
    PhoneSetupResult result;
};

/// Form navigation and submission controls do not represent persistent device settings.
static bool setupTransportField(const string& key) {
    return key == "ReturnPage" || key == "DefaultSubmit" || key == "CheckBoxManager";
}

/// Parse supported fields and retain each checkbox's original submission value.
static SetupForm setupReadForm(const string& body) {
    SetupForm form; form.values = readPhoneForm(body);
    const string lower = toLower(body); size_t position = 0;
    while ((position = lower.find("<input", position)) != string::npos) {
        const size_t end = htmlTagEnd(body, position); if (end == string::npos) break;
        auto attributes = htmlAttributes(body.substr(position, end - position + 1)); position = end + 1;
        const string type = toLower(attributes["type"]);
        if (type == "submit" || type == "button" || type == "reset") { form.values.erase(attributes["name"]); continue; }
        if (toLower(attributes["type"]) == "checkbox" && !attributes["name"].empty())
            form.checkboxes[attributes["name"]] = attributes["value"].empty() ? "ON" : attributes["value"];
    }
    return form;
}

/// Encode complete forms without interpolating inspected values into logs or scripts.
static string setupUrlEncode(const string& input) {
    static const char hex[] = "0123456789ABCDEF"; string output;
    for (unsigned char value : input) {
        if (isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~') output += value;
        else { output += '%'; output += hex[value >> 4]; output += hex[value & 15]; }
    }
    return output;
}

/// Read a supported select option without guessing firmware enumeration values.
static bool setupHasOption(const string& body, const string& field, const string& value) {
    const string lower = toLower(body); size_t position = 0;
    while ((position = lower.find("<select", position)) != string::npos) {
        const size_t end = htmlTagEnd(body, position); if (end == string::npos) return false;
        auto attrs = htmlAttributes(body.substr(position, end - position + 1)); position = end + 1;
        const size_t close = lower.find("</select", position); if (close == string::npos) return false;
        if (attrs["name"] != field) { position = close + 8; continue; }
        while ((position = lower.find("<option", position)) != string::npos && position < close) {
            const size_t last = htmlTagEnd(body, position); if (last == string::npos) return false;
            auto option = htmlAttributes(body.substr(position, last - position + 1)); position = last + 1;
            if (option["value"] == value) return true;
        }
        return false;
    }
    return false;
}

/// Encode complete forms using escaped UTF-8 keys and values.
static string setupEncode(const std::map<string, string>& values) {
    string output;
    for (const auto& field : values) { if (!output.empty()) output += '&'; output += setupUrlEncode(field.first) + "=" + setupUrlEncode(field.second); }
    return output;
}

/// Restore a percent-encoded backup component with strict malformed-input rejection.
static bool setupDecodePart(const string& input, string& output) {
    output.clear();
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '%') {
            if (index + 2 >= input.size() || !isxdigit(static_cast<unsigned char>(input[index + 1])) || !isxdigit(static_cast<unsigned char>(input[index + 2]))) return false;
            const char value = static_cast<char>(strtoul(input.substr(index + 1, 2).c_str(), NULL, 16));
            if (!value) return false; output += value; index += 2;
        } else output += input[index] == '+' ? ' ' : input[index];
    }
    return true;
}

/// Decode one bounded flat form from a current-user protected backup.
static bool setupDecode(const string& input, std::map<string, string>& values) {
    values.clear(); size_t position = 0;
    while (position < input.size() && values.size() < 512) {
        const size_t end = input.find('&', position), equal = input.find('=', position);
        if (equal == string::npos || (end != string::npos && equal > end)) return false;
        string key, value;
        if (!setupDecodePart(input.substr(position, equal - position), key) || key.empty() ||
            !setupDecodePart(input.substr(equal + 1, (end == string::npos ? input.size() : end) - equal - 1), value) || values.count(key)) return false;
        values[key] = value; if (end == string::npos) return true; position = end + 1;
    }
    return position == input.size();
}

/// Submit every preserved field, using checkbox wire semantics and the existing manager.
static string setupSubmission(const SetupForm& form, const string& path) {
    auto values = form.values;
    for (const auto& checkbox : form.checkboxes) {
        if (values[checkbox.first] == "1") values[checkbox.first] = checkbox.second;
        else values.erase(checkbox.first);
    }
    if (!form.checkboxes.empty()) {
        string manager; for (const auto& checkbox : form.checkboxes) { if (!manager.empty()) manager += ','; manager += checkbox.first; }
        values["CheckBoxManager"] = manager;
    }
    values["ReturnPage"] = path; values["DefaultSubmit"] = "Apply";
    return setupEncode(values);
}

/// Require equality of all persistent fields, including unrelated device configuration.
static bool setupSame(const std::map<string, string>& left, const std::map<string, string>& right) {
    auto a = left, b = right;
    for (auto it = a.begin(); it != a.end();) { if (setupTransportField(it->first)) it = a.erase(it); else ++it; }
    for (auto it = b.begin(); it != b.end();) { if (setupTransportField(it->first)) it = b.erase(it); else ++it; }
    return a == b;
}

/// Retarget an empty or recognized recording callback; refuse unrelated company actions.
static bool setupCallback(const string& original, const char* event, string& target) {
    const string base = "http://" + g_cfg.advertiseIp + ":" + std::to_string(g_cfg.hookPort);
    if (trim(original).empty()) { target = base + "/event?ev=" + event; return true; }
    const string source = trim(original); const size_t slash = source.find('/', 7);
    if (toLower(source.substr(0, 7)) != "http://" || slash == string::npos) return false;
    const string authority = source.substr(7, slash - 7); const size_t colon = authority.find(':');
    const string host = authority.substr(0, colon);
    const int port = colon == string::npos ? 80 : configurationInteger(authority.substr(colon + 1));
    if (!validIpv4(host) || port < 1 || port > 65535 || source.find('#') != string::npos) return false;
    // Match only the recording event contract; retain all existing macro parameters.
    const string suffix = source.substr(slash);
    if (suffix.compare(0, 7, "/event?") != 0) return false;
    bool matches = false; size_t first = 7;
    while (first < suffix.size()) {
        const size_t end = suffix.find('&', first); const string pair = suffix.substr(first, end == string::npos ? string::npos : end - first);
        const size_t equal = pair.find('=');
        if (equal != string::npos && (pair.substr(0, equal) == "ev" || pair.substr(0, equal) == "event")) {
            const string value = toLower(pair.substr(equal + 1));
            const bool correct = value == event || (string(event) == "est" && value == "established") || (string(event) == "end" && value == "terminated");
            if (!correct || matches) return false; matches = true;
        }
        if (end == string::npos) break; first = end + 1;
    }
    if (!matches) return false; target = base + suffix; return true;
}

/// Protect a complete pre-write snapshot before changing either phone page.
static string setupSaveBackup(const SetupForm& record, const SetupForm& actions) {
    const string folder = "phone-setup-backups";
    if (!ensureDir(folder)) return "";
    const string data = "PhoneRecordSetup1\n" + g_cfg.phoneIp + ":" + std::to_string(g_cfg.phonePort) + "\n" +
        setupEncode(record.values) + "\n" + setupSubmission(record, "/record.htm") + "\n" +
        setupEncode(actions.values) + "\n" + setupSubmission(actions, "/action.htm") + "\n";
    string encrypted; if (!protectLocalSecret(data, encrypted)) return "";
    const string name = folder + "\\setup-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".dpapi";
    HANDLE file = CreateFileW(utf8ToWide(name).c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return "";
    DWORD written = 0; const bool ok = WriteFile(file, encrypted.data(), static_cast<DWORD>(encrypted.size()), &written, NULL) && written == encrypted.size() && FlushFileBuffers(file);
    CloseHandle(file); return ok ? name : "";
}
