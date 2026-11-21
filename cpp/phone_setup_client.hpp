#pragma once

/// Plan/apply two supported forms, or restore one current-account encrypted snapshot.
PhoneSetupResult PhoneClient::configurePhone(bool apply, const string& expectedPlan, const string& restorePath) {
    std::lock_guard<std::mutex> guard(mutex_);
    PhoneSetupResult result;
    const auto finish = [&](const char* code, bool ok = false) {
        result.code = code; result.ok = ok;
        LOGI("PHONE_SETUP_RESULT code=%s changes=%zu applied=%d rollback=%d restored=%d", code, result.changes.size(), result.applied, result.rollbackAttempted, result.rollbackVerified);
        return result;
    };
    const PrDeviceProfile& profile = activeDeviceProfile();
    if ((profile.capabilities & PR_CAP_AUTO_SETUP) == 0 || std::strcmp(prText(profile.auth_scheme), "fanvil-nonce-md5") != 0) return finish("SETUP_UNSUPPORTED");
    if (g_offline) return finish("SETUP_OFFLINE");
    if (!g_cfg.errors().empty() || g_cfg.hookPort == 0) return finish("SETUP_CONFIG_INVALID");
    if (diagnosticSnapshot().callActive || g_sessionCount > 0) return finish("SETUP_PHONE_BUSY");
    string head, body; SetupForm beforeRecord, beforeActions; bool supportedOptions = false;
    const auto read = [&](SetupForm& record, SetupForm& actions) {
        if (!request("GET", profile.record_page, "", head, body, true) || !protectedPage(head, body)) return false;
        std::smatch status;
        if (!profile.record_status_regex[0] || !std::regex_search(body, status, std::regex(profile.record_status_regex)) || status[1].str() != std::to_string(profile.ready_state)) return false;
        record = setupReadForm(body);
        supportedOptions = setupHasOption(body, profile.field_type, profile.value_network) && setupHasOption(body, profile.field_codec, profile.value_codec_pcma);
        if (!request("GET", profile.action_page, "", head, body, true) || head.find("200") == string::npos || loginPage(body)) return false;
        actions = setupReadForm(body); return true;
    };
    const auto post = [&](const string& path, const string& payload) {
        if (diagnosticSnapshot().callActive || g_sessionCount > 0) { result.cause = "SETUP_PHONE_BUSY"; return false; }
        const bool received = request("POST", path, payload, head, body, true);
        std::istringstream statusLine(head.substr(0, head.find("\r\n"))); string protocol; int status = 0; statusLine >> protocol >> status;
        const bool denied = status == 401 || status == 403 || loginPage(body);
        const bool ok = received && status == 200 && !denied;
        if (!ok && result.cause.empty()) result.cause = denied ? "SETUP_WRITE_DENIED" : "SETUP_WRITE_FAILED";
        return ok;
    };
    if (!read(beforeRecord, beforeActions)) return finish("SETUP_PHONE_UNAVAILABLE_OR_BUSY");
    for (const char* key : {profile.field_enable, profile.field_type, profile.field_codec, profile.field_address, profile.field_port, profile.field_checkbox_manager})
        if (!key[0] || !beforeRecord.values.count(key)) return finish("SETUP_UNSUPPORTED");
    if (!beforeRecord.checkboxes.count(profile.field_enable)) return finish("SETUP_UNSUPPORTED");
    if (profile.field_default_record[0] && beforeRecord.values.count(profile.field_default_record)) return finish("SETUP_UNSUPPORTED");
    if (!supportedOptions) return finish("SETUP_UNSUPPORTED");
    for (const char* key : {profile.field_established, profile.field_terminated})
        if (!key[0] || !beforeActions.values.count(key)) return finish("SETUP_UNSUPPORTED");

    if (!restorePath.empty()) {
        std::ifstream input(utf8ToWide(restorePath), std::ios::binary); string encoded, plain;
        input.seekg(0, std::ios::end); const auto length = input.tellg();
        if (length < 0 || length > 4 * 1024 * 1024) return finish("SETUP_BACKUP_INVALID");
        input.seekg(0); encoded.resize(static_cast<size_t>(length));
        if (!input.read(&encoded[0], length)) return finish("SETUP_BACKUP_INVALID");
        if (encoded.size() > 4 * 1024 * 1024 || encoded.compare(0, 6, "dpapi:") != 0 || !unprotectLocalSecret(encoded, plain)) return finish("SETUP_BACKUP_INVALID");
        std::istringstream backup(plain); string marker, target, recordMap, recordPost, actionMap, actionPost;
        if (!std::getline(backup, marker) || !std::getline(backup, target) || !std::getline(backup, recordMap) || !std::getline(backup, recordPost) ||
            !std::getline(backup, actionMap) || !std::getline(backup, actionPost) || marker != "PhoneRecordSetup1" || target != phoneAddress() + ":" + std::to_string(g_cfg.phonePort)) return finish("SETUP_BACKUP_INVALID");
        std::map<string, string> recordExpected, actionExpected;
        if (!setupDecode(recordMap, recordExpected) || !setupDecode(actionMap, actionExpected)) return finish("SETUP_BACKUP_INVALID");
        result.backup = setupSaveBackup(beforeRecord, beforeActions);
        if (result.backup.empty()) return finish("SETUP_BACKUP_FAILED");
        const bool actionOk = post(profile.action_page, actionPost), recordOk = post(profile.record_page, recordPost);
        SetupForm record, actions;
        if (read(record, actions) && setupSame(recordExpected, record.values) && setupSame(actionExpected, actions.values)) { result.applied = true; result.cause.clear(); if (!completePhonePairing(false)) return finish("SETUP_PAIRING_SAVE_FAILED"); return finish("SETUP_RESTORED", true); }
        LOGW("PHONE_SETUP_RESTORE_FAILED record_ack=%d action_ack=%d", recordOk, actionOk);
        return finish("SETUP_RESTORE_FAILED");
    }

    if (!ensurePhonePairingToken()) return finish("SETUP_PAIRING_SAVE_FAILED");
    SetupForm afterRecord = beforeRecord, afterActions = beforeActions;
    afterRecord.values[profile.field_enable] = profile.value_enabled; afterRecord.values[profile.field_type] = profile.value_network;
    const string codec = toLower(beforeRecord.values[profile.field_codec]);
    afterRecord.values[profile.field_codec] = codec == toLower(profile.value_codec_pcmu) ? profile.value_codec_pcmu : profile.value_codec_pcma;
    afterRecord.values[profile.field_address] = g_cfg.advertiseIp;
    afterRecord.values[profile.field_port] = std::to_string(g_cfg.controlPort);
    std::vector<std::pair<string, string>> callbacks{{profile.field_established, prCanonicalToken(profile, PR_EVENT_ESTABLISHED)}, {profile.field_terminated, prCanonicalToken(profile, PR_EVENT_TERMINATED)}};
    // Startup and IP-change URLs are optional. Empty or already-owned values can be written;
    // an existing foreign URL stays in place so one-click setup can still pair the call events.
    if (g_cfg.phoneAddressMode == "dhcp") {
        const std::pair<const char*, const char*> optionalCallbacks[] = {{profile.field_setup_completed, prCanonicalToken(profile, PR_EVENT_READY)}, {profile.field_ip_change, prCanonicalToken(profile, PR_EVENT_ADDRESS)}};
        for (const auto& extra : optionalCallbacks) {
            const auto existing = beforeActions.values.find(extra.first);
            string target;
            if (existing != beforeActions.values.end() && setupCallback(existing->second, extra.second, target)) callbacks.emplace_back(extra.first, extra.second);
        }
    }
    for (const auto& callback : callbacks) {
        string target;
        if (!setupCallback(beforeActions.values.at(callback.first), callback.second.c_str(), target)) return finish("SETUP_CALLBACK_CONFLICT");
        afterActions.values[callback.first] = target;
    }
    for (const auto& field : afterRecord.values) if (!setupTransportField(field.first) && beforeRecord.values.at(field.first) != field.second) result.changes.push_back(field.first);
    for (const auto& field : afterActions.values) if (!setupTransportField(field.first) && beforeActions.values.at(field.first) != field.second) result.changes.push_back(field.first);
    result.planId = Md5::hex(phoneAddress() + ":" + std::to_string(g_cfg.phonePort) + "\n" + setupEncode(beforeRecord.values) + "\n" +
        setupEncode(beforeActions.values) + "\n" + setupEncode(afterRecord.values) + "\n" + setupEncode(afterActions.values));
    if (!apply) return finish(result.changes.empty() ? "SETUP_ALREADY_MATCHED" : "SETUP_PLAN_READY", true);
    if (expectedPlan.empty() || expectedPlan != result.planId) return finish("SETUP_PLAN_STALE");
    if (result.changes.empty()) { if (!completePhonePairing(true)) return finish("SETUP_PAIRING_SAVE_FAILED"); return finish("SETUP_ALREADY_MATCHED", true); }
    result.backup = setupSaveBackup(beforeRecord, beforeActions);
    if (result.backup.empty()) return finish("SETUP_BACKUP_FAILED");
    const bool recordChange = !setupSame(beforeRecord.values, afterRecord.values), actionChange = !setupSame(beforeActions.values, afterActions.values);
    bool recordTouched = false, actionTouched = false;
    bool sent = true;
    if (recordChange) { recordTouched = true; sent = post(profile.record_page, setupSubmission(afterRecord, profile.record_page)); }
    if (sent && actionChange) { actionTouched = true; sent = post(profile.action_page, setupSubmission(afterActions, profile.action_page)); }
    SetupForm record, actions;
    if (sent && read(record, actions) && setupSame(afterRecord.values, record.values) && setupSame(afterActions.values, actions.values)) {
        result.applied = true; if (!completePhonePairing(true)) return finish("SETUP_PAIRING_SAVE_FAILED"); return finish("SETUP_VERIFIED", true);
    }
    result.rollbackAttempted = true;
    if (result.cause.empty()) result.cause = "SETUP_READBACK_FAILED";
    LOGW("PHONE_SETUP_ROLLBACK_BEGIN record=%d action=%d", recordTouched, actionTouched);
    if (recordState(true) != 4) return finish("SETUP_FAILED_RESTORE_REQUIRED");
    if (actionTouched) post(profile.action_page, setupSubmission(beforeActions, profile.action_page));
    if (recordTouched) post(profile.record_page, setupSubmission(beforeRecord, profile.record_page));
    result.rollbackVerified = read(record, actions) && setupSame(beforeRecord.values, record.values) && setupSame(beforeActions.values, actions.values);
    return finish(result.rollbackVerified ? "SETUP_FAILED_RESTORED" : "SETUP_FAILED_RESTORE_REQUIRED");
}
