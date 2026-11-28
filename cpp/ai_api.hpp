#pragma once
#ifdef _WIN64
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

/// Largest assistant reply kept from one non-streaming API response.
static const size_t kAiApiTextLimit = 2 * 1024 * 1024;
/// Anthropic Messages requires max_tokens; this is the fixed request value, not an input truncation.
static const int kAiApiMaxTokens = 8192;

/// Active WinHTTP request published so cancellation can close it from the UI thread.
struct AiHttpSlot {
    /// Guards publication of the request handle. Held only while swapping the pointer.
    std::mutex* mutex = nullptr;
    /// Nullable slot owned by the analysis coordinator.
    HINTERNET* request = nullptr;
    /// Set by the UI before the handle is closed.
    std::atomic<bool>* cancel = nullptr;
};

/// One completed HTTP exchange. `detail` is already safe to store; it never contains the API key.
struct AiHttpResult {
    /// Stable step: complete, provider_error, api_failed, timeout, cancelled or output_limit.
    string step;
    /// HTTP status or WinHTTP error number, empty when no request was sent.
    string code;
    /// Response body for a 2xx reply, otherwise empty.
    string body;
    /// Short redacted diagnostic for the local stderr file.
    string detail;
};

/// True when `id` is one of the three supported request protocols.
static bool aiApiFormat(const string& id) {
    return id == "openai-chat" || id == "openai-responses" || id == "anthropic";
}

/// Accept a model id that can be placed in JSON without changing the request shape.
static bool aiApiModelId(const string& model) {
    if (model.empty() || model.size() > 256) return false;
    for (unsigned char byte : model) {
        const bool ok = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            byte == '.' || byte == '_' || byte == '-' || byte == ':' || byte == '/' || byte == '+' || byte == '@';
        if (!ok) return false;
    }
    return true;
}

/// Reject control characters so a saved key cannot break an HTTP header.
static bool aiApiKeyOk(const string& key) {
    if (key.empty() || key.size() > 4096) return false;
    for (unsigned char byte : key) if (byte < 0x20 || byte == 0x7f) return false;
    return true;
}

/// Remove a copied key from text that will be written next to a recording.
static string aiApiRedact(string text, const string& key) {
    if (key.size() < 4) return text;
    for (size_t pos = 0; (pos = text.find(key, pos)) != string::npos; ) {
        text.replace(pos, key.size(), "<redacted>");
        pos += 10;
    }
    return text;
}

/// Compare an ASCII URL suffix without treating a longer token as a match.
static bool aiApiEnds(const string& value, const char* suffix) {
    const size_t length = strlen(suffix);
    if (value.size() < length) return false;
    for (size_t index = 0; index < length; ++index) {
        char left = value[value.size() - length + index], right = suffix[index];
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left + 32);
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right + 32);
        if (left != right) return false;
    }
    return true;
}

/// Trim spaces typed around a pasted base URL.
static string aiApiTrim(string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}

/// Refuse query secrets; an API key belongs in the encrypted field, not the saved address.
static bool aiApiQuerySafe(const string& query) {
    string lower = query;
    for (char& byte : lower) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte + 32);
    return lower.find("api_key=") == string::npos && lower.find("apikey=") == string::npos &&
        lower.find("api-key=") == string::npos && lower.find("token=") == string::npos &&
        lower.find("secret=") == string::npos && lower.find("key=") == string::npos;
}

/// Append the protocol path. A base that already ends at the endpoint is left unchanged.
static string aiApiJoinPath(const string& format, string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (format == "openai-chat") {
        if (aiApiEnds(path, "/chat/completions")) return path;
        if (aiApiEnds(path, "/v1")) return path + "/chat/completions";
        return path + "/v1/chat/completions";
    }
    if (format == "openai-responses") {
        if (aiApiEnds(path, "/responses")) return path;
        if (aiApiEnds(path, "/v1")) return path + "/responses";
        return path + "/v1/responses";
    }
    if (aiApiEnds(path, "/messages")) return path;
    if (aiApiEnds(path, "/v1")) return path + "/messages";
    return path + "/v1/messages";
}

/// True for the only clear-text hosts that may receive a local model request.
static bool aiApiLoopback(wstring host) {
    for (wchar_t& ch : host) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + 32);
    return host == L"localhost" || host == L"127.0.0.1" || host == L"::1" || host == L"[::1]";
}

/// Build the final request URL. `error` is `api_url_invalid` when the address is rejected.
static bool aiApiEndpoint(const string& format, const string& base, string& url, string& error) {
    url.clear();
    error = "api_url_invalid";
    if (!aiApiFormat(format)) return false;
    const string trimmed = aiApiTrim(base);
    if (trimmed.empty() || trimmed.size() > 2048) return false;
    for (unsigned char byte : trimmed) if (byte <= 0x20 || byte >= 0x7f || byte == '\\' || byte == '#' || byte == '@') return false;
    const size_t queryAt = trimmed.find('?');
    const string query = queryAt == string::npos ? "" : trimmed.substr(queryAt);
    string root = queryAt == string::npos ? trimmed : trimmed.substr(0, queryAt);
    if (!query.empty() && !aiApiQuerySafe(query)) return false;
    const size_t scheme = root.find("://");
    if (scheme == string::npos || scheme == 0) return false;
    const size_t pathAt = root.find('/', scheme + 3);
    const string prefix = pathAt == string::npos ? root : root.substr(0, pathAt);
    string path = pathAt == string::npos ? "" : root.substr(pathAt);
    url = prefix + aiApiJoinPath(format, path) + query;
    const wstring wide = utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 || parts.nPort == 0) return false;
    const wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopbackHttp = parts.nScheme == INTERNET_SCHEME_HTTP && aiApiLoopback(host);
    if (!https && !loopbackHttp) return false;
    error.clear();
    return true;
}

/// Derive GET /models from a joined chat, responses or messages URL.
static string aiApiModelsUrl(const string& endpoint) {
    const size_t queryAt = endpoint.find('?');
    string path = queryAt == string::npos ? endpoint : endpoint.substr(0, queryAt);
    const string query = queryAt == string::npos ? "" : endpoint.substr(queryAt);
    const char* suffixes[] = {"/chat/completions", "/responses", "/messages"};
    for (const char* suffix : suffixes) if (aiApiEnds(path, suffix)) {
        path.erase(path.size() - strlen(suffix));
        return path + "/models" + query;
    }
    return "";
}

/// Encode one user turn. The assembled prompt already contains the instructions.
static string aiApiRequestBody(const string& format, const string& model, const string& prompt) {
    if (format == "openai-responses") return "{\"model\":" + jsonQuoted(model) + ",\"input\":" + jsonQuoted(prompt) + "}";
    if (format == "anthropic") return "{\"model\":" + jsonQuoted(model) + ",\"max_tokens\":" + std::to_string(kAiApiMaxTokens) + ",\"messages\":[{\"role\":\"user\",\"content\":" + jsonQuoted(prompt) + "}]}";
    return "{\"model\":" + jsonQuoted(model) + ",\"messages\":[{\"role\":\"user\",\"content\":" + jsonQuoted(prompt) + "}]}";
}

/// Skip whitespace inside one JSON value.
static void aiJsonWs(const string& json, size_t& pos) {
    while (pos < json.size()) {
        const unsigned char byte = json[pos];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') break;
        ++pos;
    }
}

/// Skip one JSON value so a key search stays on the current object. Depth is bounded.
static bool aiJsonSkip(const string& json, size_t& pos, int depth = 0) {
    if (depth > 32 || pos >= json.size()) return false;
    const char kind = json[pos];
    if (kind == '"') { string discarded; return readJsonString(json, pos, discarded, kAiApiTextLimit); }
    if (kind == '{' || kind == '[') {
        const char close = kind == '{' ? '}' : ']';
        ++pos;
        while (pos < json.size()) {
            aiJsonWs(json, pos);
            if (pos < json.size() && json[pos] == close) { ++pos; return true; }
            if (kind == '{') {
                string name;
                if (!readJsonString(json, pos, name, 4096)) return false;
                aiJsonWs(json, pos);
                if (pos >= json.size() || json[pos++] != ':') return false;
                aiJsonWs(json, pos);
            }
            if (!aiJsonSkip(json, pos, depth + 1)) return false;
            aiJsonWs(json, pos);
            if (pos < json.size() && json[pos] == ',') ++pos;
        }
        return false;
    }
    if (json.compare(pos, 4, "true") == 0 || json.compare(pos, 4, "null") == 0) { pos += 4; return true; }
    if (json.compare(pos, 5, "false") == 0) { pos += 5; return true; }
    if (kind == '-' || (kind >= '0' && kind <= '9')) {
        ++pos;
        while (pos < json.size()) {
            const unsigned char byte = json[pos];
            if (!((byte >= '0' && byte <= '9') || byte == '.' || byte == 'e' || byte == 'E' || byte == '+' || byte == '-')) break;
            ++pos;
        }
        return true;
    }
    return false;
}

/// Find a direct key of the object at `objectPos` and return the position of its value.
static bool aiJsonFind(const string& json, size_t objectPos, const char* key, size_t& valuePos) {
    if (objectPos >= json.size() || json[objectPos] != '{') return false;
    size_t pos = objectPos + 1;
    while (pos < json.size()) {
        aiJsonWs(json, pos);
        if (pos < json.size() && json[pos] == '}') return false;
        string field;
        if (!readJsonString(json, pos, field, 4096)) return false;
        aiJsonWs(json, pos);
        if (pos >= json.size() || json[pos++] != ':') return false;
        aiJsonWs(json, pos);
        if (field == key) { valuePos = pos; return valuePos < json.size(); }
        if (!aiJsonSkip(json, pos)) return false;
        aiJsonWs(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return false;
}

/// Read a JSON string value at `valuePos` into `text`.
static bool aiJsonString(const string& json, size_t valuePos, string& text) {
    if (valuePos >= json.size() || json[valuePos] != '"') return false;
    return readJsonString(json, valuePos, text, kAiApiTextLimit);
}

/// Keep text parts and skip thinking or reasoning blocks.
static bool aiApiTextPart(const string& type) {
    return type.empty() || type == "text" || type == "output_text";
}

/// Concatenate accepted text parts from one content array.
static bool aiApiAppendTexts(const string& json, size_t arrayPos, string& output) {
    if (arrayPos >= json.size() || json[arrayPos] != '[') return false;
    size_t pos = arrayPos + 1;
    while (pos < json.size()) {
        aiJsonWs(json, pos);
        if (pos < json.size() && json[pos] == ']') return true;
        if (pos >= json.size() || json[pos] != '{') return false;
        size_t typePos = 0, textPos = 0;
        string type, text;
        if (aiJsonFind(json, pos, "type", typePos)) aiJsonString(json, typePos, type);
        if (aiApiTextPart(type) && aiJsonFind(json, pos, "text", textPos) && aiJsonString(json, textPos, text)) output += text;
        if (!aiJsonSkip(json, pos)) return false;
        aiJsonWs(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return false;
}

/// Return the first object in an array, or false when the array is empty or not objects.
static bool aiJsonFirstObject(const string& json, size_t arrayPos, size_t& objectPos) {
    if (arrayPos >= json.size() || json[arrayPos] != '[') return false;
    size_t pos = arrayPos + 1;
    aiJsonWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    objectPos = pos;
    return true;
}

/// Extract the assistant text for the selected protocol. Thinking blocks are omitted.
static bool aiApiExtract(const string& format, const string& payload, string& text) {
    text.clear();
    size_t root = payload.find_first_not_of(" \t\r\n");
    if (root != string::npos && root + 2 < payload.size() && static_cast<unsigned char>(payload[root]) == 0xef) root += 3;
    if (root == string::npos || payload[root] != '{') return false;
    size_t value = 0;
    if (format == "openai-responses") {
        if (aiJsonFind(payload, root, "output_text", value) && aiJsonString(payload, value, text) && !text.empty()) return true;
        text.clear();
        if (!aiJsonFind(payload, root, "output", value)) return false;
        size_t pos = value + 1;
        while (pos < payload.size()) {
            aiJsonWs(payload, pos);
            if (pos < payload.size() && payload[pos] == ']') break;
            if (pos >= payload.size() || payload[pos] != '{') return false;
            size_t typePos = 0, contentPos = 0;
            string type;
            if (aiJsonFind(payload, pos, "type", typePos)) aiJsonString(payload, typePos, type);
            if (type == "message" && aiJsonFind(payload, pos, "content", contentPos)) aiApiAppendTexts(payload, contentPos, text);
            if (!aiJsonSkip(payload, pos)) return false;
            aiJsonWs(payload, pos);
            if (pos < payload.size() && payload[pos] == ',') ++pos;
        }
        return !text.empty();
    }
    if (format == "anthropic") {
        if (!aiJsonFind(payload, root, "content", value)) return false;
        return aiApiAppendTexts(payload, value, text) && !text.empty();
    }
    if (!aiJsonFind(payload, root, "choices", value)) return false;
    size_t choice = 0;
    if (!aiJsonFirstObject(payload, value, choice)) return false;
    size_t message = 0, content = 0;
    if (!aiJsonFind(payload, choice, "message", message) || payload[message] != '{') return false;
    if (!aiJsonFind(payload, message, "content", content)) return false;
    if (payload[content] == '"') return aiJsonString(payload, content, text);
    if (payload[content] == '[') return aiApiAppendTexts(payload, content, text) && !text.empty();
    return false;
}

/// Read a provider error string when the body uses the common error object.
static bool aiApiErrorMessage(const string& payload, string& message) {
    message.clear();
    size_t root = payload.find_first_not_of(" \t\r\n");
    if (root == string::npos || payload[root] != '{') return false;
    size_t value = 0;
    if (aiJsonFind(payload, root, "error", value) && value < payload.size() && payload[value] == '{') {
        size_t nested = 0;
        if (aiJsonFind(payload, value, "message", nested) && aiJsonString(payload, nested, message)) return true;
    }
    return aiJsonFind(payload, root, "message", value) && aiJsonString(payload, value, message);
}

/// Collect model ids from a `{ "data": [ { "id": "..." } ] }` list, at most 200 unique values.
static std::vector<string> aiApiModelIds(const string& payload) {
    std::vector<string> ids;
    size_t root = payload.find_first_not_of(" \t\r\n");
    size_t data = 0;
    if (root == string::npos || payload[root] != '{' || !aiJsonFind(payload, root, "data", data) || payload[data] != '[') return ids;
    size_t pos = data + 1;
    while (ids.size() < 200 && pos < payload.size()) {
        aiJsonWs(payload, pos);
        if (pos < payload.size() && payload[pos] == ']') break;
        if (pos >= payload.size() || payload[pos] != '{') break;
        size_t idPos = 0;
        string id;
        if (aiJsonFind(payload, pos, "id", idPos) && aiJsonString(payload, idPos, id) && aiApiModelId(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        if (!aiJsonSkip(payload, pos)) break;
        aiJsonWs(payload, pos);
        if (pos < payload.size() && payload[pos] == ',') ++pos;
    }
    return ids;
}

/// Drop the published request handle without closing it twice after cancellation.
static void aiHttpRelease(AiHttpSlot& slot, HINTERNET& request) {
    if (!request || !slot.mutex || !slot.request) { request = NULL; return; }
    std::lock_guard<std::mutex> lock(*slot.mutex);
    if (*slot.request == request) { WinHttpCloseHandle(request); *slot.request = NULL; }
    request = NULL;
}

/// Send one non-streaming request. The key is a header only and is removed from `detail`.
static AiHttpResult aiHttpExchange(const string& url, const wchar_t* method, const string& format, const string& key, const string& payload, unsigned timeoutSec, AiHttpSlot slot) {
    AiHttpResult result;
    if (slot.cancel && slot.cancel->load()) { result.step = "cancelled"; return result; }
    if (!key.empty() && !aiApiKeyOk(key)) { result.step = "api_failed"; result.detail = "invalid key"; return result; }
    if (payload.size() > 8 * 1024 * 1024) { result.step = "api_failed"; result.detail = "request too large"; return result; }
    const wstring wide = utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0) { result.step = "api_failed"; result.detail = "invalid url"; return result; }
    const wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const DWORD access = aiApiLoopback(host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    HINTERNET session = WinHttpOpen(L"PhoneRecord", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { result.step = "api_failed"; result.code = std::to_string(GetLastError()); return result; }
    const DWORD receive = timeoutSec == 0 ? 30000 : timeoutSec * 1000;
    WinHttpSetTimeouts(session, 10000, 15000, receive, receive);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    wstring target = parts.dwUrlPathLength ? wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, target.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!request) {
        result.step = "api_failed"; result.code = std::to_string(GetLastError());
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }
    bool owned = false;
    if (slot.mutex && slot.request) {
        std::lock_guard<std::mutex> lock(*slot.mutex);
        if (slot.cancel && slot.cancel->load()) { WinHttpCloseHandle(request); request = NULL; }
        else { *slot.request = request; owned = true; }
    }
    if (!request) {
        result.step = "cancelled";
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH, disableAuth = WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
    // Custom bearer and x-api-key values are ordinary headers. Leaving authentication enabled lets WinHTTP drop them on cleartext loopback.
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disableAuth, sizeof(disableAuth));
    wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (format == "anthropic") headers += L"anthropic-version: 2023-06-01\r\n";
    if (!key.empty()) headers += format == "anthropic" ? L"x-api-key: " + utf8ToWide(key) + L"\r\n" : L"Authorization: Bearer " + utf8ToWide(key) + L"\r\n";
    const bool headersAdded = WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) != FALSE;
    const bool posted = headersAdded && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0) && WinHttpReceiveResponse(request, NULL);
    if (posted) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        result.code = std::to_string(status);
        char buffer[16384];
        DWORD received = 0;
        bool limited = false;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &received)) {
            if (!received) break;
            if (slot.cancel && slot.cancel->load()) { result.step = "cancelled"; break; }
            if (result.body.size() + received > kAiApiTextLimit) { limited = true; break; }
            result.body.append(buffer, received);
        }
        if (result.step.empty()) {
            if (limited) result.step = "output_limit";
            else if (status >= 200 && status < 300) result.step = "complete";
            else {
                result.step = "provider_error";
                string message;
                result.detail = "HTTP " + result.code;
                if (aiApiErrorMessage(result.body, message) && !message.empty()) result.detail += " " + message.substr(0, 300);
                result.body.clear();
            }
        }
    } else if (slot.cancel && slot.cancel->load()) result.step = "cancelled";
    else {
        const DWORD failure = GetLastError();
        result.code = std::to_string(failure);
        if (failure == ERROR_WINHTTP_OPERATION_CANCELLED) result.step = "cancelled";
        else if (failure == ERROR_WINHTTP_TIMEOUT) result.step = "timeout";
        else result.step = "api_failed";
        result.detail = "winhttp " + result.code;
    }
    result.detail = aiApiRedact(result.detail, key);
    if (owned) aiHttpRelease(slot, request);
    else if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}
#endif
