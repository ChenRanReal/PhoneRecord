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
    if (g_offline) return finish("SETUP_OFFLINE");
    if (!g_cfg.errors().empty() || g_cfg.hookPort == 0) return finish("SETUP_CONFIG_INVALID");
    if (diagnosticSnapshot().callActive || g_sessionCount > 0) return finish("SETUP_PHONE_BUSY");
    string head, body; SetupForm beforeRecord, beforeActions; bool supportedOptions = false;
    const auto read = [&](SetupForm& record, SetupForm& actions) {
        if (!request("GET", "/record.htm", "", head, body, true) || !protectedPage(head, body)) return false;
        std::smatch status;
        if (!std::regex_search(body, status, std::regex("recordStatus\\s*=\\s*\"\\s*(\\d+)")) || status[1] != "4") return false;
        record = setupReadForm(body);
        supportedOptions = setupHasOption(body, "CSE_RecordType_RW", "1") && setupHasOption(body, "CSE_RecordVoiceCodec_R", "PCMA");
        if (!request("GET", "/action.htm", "", head, body, true) || head.find("200") == string::npos || loginPage(body)) return false;
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
    for (const char* key : {"EnableRecord_RW", "CSE_RecordType_RW", "CSE_RecordVoiceCodec_R", "SYS_RecordAddr_RW", "SYS_RecordPort_RW", "CheckBoxManager"})
        if (!beforeRecord.values.count(key)) return finish("SETUP_UNSUPPORTED");
    if (!beforeRecord.checkboxes.count("EnableRecord_RW")) return finish("SETUP_UNSUPPORTED");
    if (beforeRecord.values.count("DefaultRecord")) return finish("SETUP_UNSUPPORTED");
    if (!supportedOptions) return finish("SETUP_UNSUPPORTED");
    for (const char* key : {"AURL_CallEstablished_RW", "AURL_CallTerminated_RW"})
        if (!beforeActions.values.count(key)) return finish("SETUP_UNSUPPORTED");

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
        const bool actionOk = post("/action.htm", actionPost), recordOk = post("/record.htm", recordPost);
        SetupForm record, actions;
        if (read(record, actions) && setupSame(recordExpected, record.values) && setupSame(actionExpected, actions.values)) { result.applied = true; result.cause.clear(); if (!completePhonePairing(false)) return finish("SETUP_PAIRING_SAVE_FAILED"); return finish("SETUP_RESTORED", true); }
        LOGW("PHONE_SETUP_RESTORE_FAILED record_ack=%d action_ack=%d", recordOk, actionOk);
        return finish("SETUP_RESTORE_FAILED");
    }

    if (!ensurePhonePairingToken()) return finish("SETUP_PAIRING_SAVE_FAILED");
    SetupForm afterRecord = beforeRecord, afterActions = beforeActions;
    afterRecord.values["EnableRecord_RW"] = "1"; afterRecord.values["CSE_RecordType_RW"] = "1";
    const string codec = toLower(beforeRecord.values["CSE_RecordVoiceCodec_R"]);
    afterRecord.values["CSE_RecordVoiceCodec_R"] = codec == "pcmu" ? "PCMU" : "PCMA";
    afterRecord.values["SYS_RecordAddr_RW"] = g_cfg.advertiseIp;
    afterRecord.values["SYS_RecordPort_RW"] = std::to_string(g_cfg.controlPort);
    std::vector<std::pair<string, string>> callbacks{{"AURL_CallEstablished_RW", "est"}, {"AURL_CallTerminated_RW", "end"}};
    // Startup and IP-change URLs are optional. Empty or already-owned values can be written;
    // an existing foreign URL stays in place so one-click setup can still pair the call events.
    if (g_cfg.phoneAddressMode == "dhcp") {
        const std::pair<const char*, const char*> optionalCallbacks[] = {{"AURL_SetupCompleted_RW", "ready"}, {"AURL_IpChange_RW", "ip"}};
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
    if (recordChange) { recordTouched = true; sent = post("/record.htm", setupSubmission(afterRecord, "/record.htm")); }
    if (sent && actionChange) { actionTouched = true; sent = post("/action.htm", setupSubmission(afterActions, "/action.htm")); }
    SetupForm record, actions;
    if (sent && read(record, actions) && setupSame(afterRecord.values, record.values) && setupSame(afterActions.values, actions.values)) {
        result.applied = true; if (!completePhonePairing(true)) return finish("SETUP_PAIRING_SAVE_FAILED"); return finish("SETUP_VERIFIED", true);
    }
    result.rollbackAttempted = true;
    if (result.cause.empty()) result.cause = "SETUP_READBACK_FAILED";
    LOGW("PHONE_SETUP_ROLLBACK_BEGIN record=%d action=%d", recordTouched, actionTouched);
    if (recordState(true) != 4) return finish("SETUP_FAILED_RESTORE_REQUIRED");
    if (actionTouched) post("/action.htm", setupSubmission(beforeActions, "/action.htm"));
    if (recordTouched) post("/record.htm", setupSubmission(beforeRecord, "/record.htm"));
    result.rollbackVerified = read(record, actions) && setupSame(beforeRecord.values, record.values) && setupSame(beforeActions.values, actions.values);
    return finish(result.rollbackVerified ? "SETUP_FAILED_RESTORED" : "SETUP_FAILED_RESTORE_REQUIRED");
}
