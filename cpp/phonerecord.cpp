// PhoneRecord 2 - Fanvil X3S 原生 C++ 通话录音工具
//
// 构建入口：cpp/build.bat。单个 x86 exe，静态链接 CRT，原生 Windows 界面。
// TCP 控制、UDP 媒体、HTTP 事件均有独立诊断证据；控制会话不等于音频成功。
// 配套头文件分别维护诊断、接收、话机客户端、原生界面与资源。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(_WIN64)
#error PhoneRecord release manifests require an x86 build. Select the x86 MSVC tools.
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <regex>
#include <wincrypt.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ===========================================================================
// 基础工具
// ===========================================================================

typedef std::string  string;
typedef std::wstring wstring;

#include "diagnostics.hpp"


// 已分配的控制会话数量，仅用于去重与退出协调；有效 RTP 另由诊断状态统计。
static std::atomic<long> g_sessionCount{0};

/// Serializes admission of new calls/control sessions against an explicit update shutdown.
static std::mutex g_updateAdmission;
/// Protected by g_updateAdmission; blocks new work only during a user-confirmed installation.
static bool g_updateInstalling = false;
/// A replacement child accepts no new calls until its native startup has been acknowledged.
static bool g_updateStartupPending = false;
/// Established-call acknowledgements which have not yet entered the command coordinator.
static std::atomic<int> g_pendingCallEvents{0};

static string nowStamp(const char* fmt = "%Y-%m-%d %H:%M:%S") {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (fmt && strcmp(fmt, "%Y-%m-%d %H:%M:%S") == 0) return buf;

    char file[64];
    snprintf(file, sizeof(file), "%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return file;
}

/// Format one technical diagnostic event; central logging redacts and bounds output.
static void logLine(const char* level, const char* fmt, ...) {
    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    g_diagnosticLog.write(level, msg);
}

#define LOGI(...) logLine("INFO", __VA_ARGS__)
#define LOGW(...) logLine("WARN", __VA_ARGS__)
#define LOGE(...) logLine("ERROR", __VA_ARGS__)
#define LOGD(...) logLine("DEBUG", __VA_ARGS__)

static wstring utf8ToWide(const string& text) {
    if (text.empty()) return wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (need <= 0) return wstring();
    wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], need);
    return out;
}

static string wideToUtf8(const wstring& text) {
    if (text.empty()) return string();
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (need <= 0) return string();
    string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], need, NULL, NULL);
    return out;
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string toLower(string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool fileExists(const string& path) {
    DWORD attr = GetFileAttributesW(utf8ToWide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/// Create a local output directory and parents; report failure to the caller.
static bool ensureDir(const string& path) {
    if (path.empty()) return false;
    const wstring input = utf8ToWide(path);
    const DWORD length = GetFullPathNameW(input.c_str(), 0, NULL, NULL);
    if (!length) return false;
    wstring absolute(length, L'\0');
    GetFullPathNameW(input.c_str(), length, &absolute[0], NULL);
    int result = SHCreateDirectoryExW(NULL, absolute.c_str(), NULL);
    const DWORD attributes = GetFileAttributesW(absolute.c_str());
    const bool ok = (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS) && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!ok) SetLastError(result);
    return ok;
}

// ===========================================================================
// G.711 解码
// ===========================================================================

static int16_t g_alaw[256];
static int16_t g_ulaw[256];

static void initG711() {
    for (int i = 0; i < 256; i++) {
        int a = i ^ 0x55;
        int t = (a & 0x0F) << 4;
        int seg = (a & 0x70) >> 4;
        if (seg == 0)      t += 8;
        else if (seg == 1) t += 0x108;
        else               t = (t + 0x108) << (seg - 1);
        g_alaw[i] = (int16_t)((a & 0x80) ? t : -t);
    }
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        g_ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
}

// ===========================================================================
// RTP 解析
// ===========================================================================

/// Bounded RTP payload location and header evidence for sequence/media-clock diagnostics.
struct RtpPacket {
    int  payloadType = 0;
    int  sequence = 0;
    int  payloadOffset = 0;
    int  payloadSize = 0;
    /// Sender's 32-bit media sampling clock, independent of packet arrival time.
    uint32_t timestamp = 0;
    /// Sender's synchronization source; sequence numbers belong to this source.
    uint32_t ssrc = 0;
};

/// Validate RTPv2 and retain payload bounds, sequence, sampling clock and source.
static bool parseRtp(const uint8_t* data, int size, RtpPacket& out) {
    if (size < 12) return false;
    if ((data[0] >> 6) != 2) return false;

    int csrcCount = data[0] & 0x0F;
    bool extension = (data[0] & 0x10) != 0;
    bool padding   = (data[0] & 0x20) != 0;

    out.payloadType = data[1] & 0x7F;
    out.sequence = (data[2] << 8) | data[3];
    out.timestamp = static_cast<uint32_t>(data[4]) << 24 | static_cast<uint32_t>(data[5]) << 16
                  | static_cast<uint32_t>(data[6]) << 8 | data[7];
    out.ssrc = static_cast<uint32_t>(data[8]) << 24 | static_cast<uint32_t>(data[9]) << 16
             | static_cast<uint32_t>(data[10]) << 8 | data[11];

    int offset = 12 + csrcCount * 4;
    if (extension) {
        if (size < offset + 4) return false;
        int extWords = (data[offset + 2] << 8) | data[offset + 3];
        offset += 4 + extWords * 4;
    }
    if (size < offset) return false;

    int end = size;
    if (padding) {
        if (end <= offset) return false;
        int pad = data[end - 1];
        if (pad <= 0 || pad > end - offset) return false;
        end -= pad;
    }

    out.payloadOffset = offset;
    out.payloadSize = end - offset;
    return out.payloadSize > 0;
}

// ===========================================================================
// WAV 写入
// ===========================================================================

#include "wav_writer.hpp"

// ===========================================================================
// 配置（INI 风格 key=value，纯 ASCII 键）
// ===========================================================================

#include "config_security.hpp"
#include "phone_address_config.hpp"
#include "recording_storage.hpp"

/// Immutable runtime configuration; GUI saves a separate validated copy for restart.
struct Config {
    int    controlPort = 10000;
    int    udpPortStart = 20000;
    int    udpPortEnd = 20100;
    int    hookPort = 18080;
    int    uiPort = 8090;
    int    idleTimeoutSec = 5;
    /// Idle-only periodic health checks; zero disables the scheduler.
    int    healthIntervalSec = 60;
    /// Close button behaviour: tray, minimize or exit.
    string closeAction = "tray";
    /// Tray balloon notifications can be disabled without affecting recording.
    int trayNotifications = 0;
    string advertiseIp = "";
    string phoneIp = "";
    /// Address authority: fixed IPv4, on-link MAC discovery, or managed DNS across subnets.
    string phoneAddressMode = "fixed";
    /// Required stable Ethernet identity for same-subnet DHCP discovery.
    string phoneMac;
    int    phonePort = 80;
    string phoneUser = "";
    string phonePass = "";
    string recordDir = "recordings";
    /// Folder policy for new recordings; old files remain discoverable under the same root.
    string recordFolderMode = "flat";
    /// Relative calendar template retained when a preset is selected.
    string recordFolderPattern = "{yyyy}/{MM}/{dd}";
    string ledgerFile = "recordings\\calls.csv";
    string directoryFile = "directory.json";
    /// Failed credential decryption must not masquerade as a wrong phone password.
    bool secretError = false;
    /// Sectioned INI input is rejected because the established format is flat.
    bool formatError = false;
    /// Preserve unknown flat keys when this version saves known settings.
    std::map<string, string> extra;

    /// Read the existing flat key/value format, including legacy plaintext passwords.
    void load(const string& path) {
        std::ifstream in(utf8ToWide(path).c_str());
        if (!in) {
            LOGW("配置文件 %s 不存在，使用默认值", path.c_str());
            return;
        }
        string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line[0] == '[') { formatError = true; continue; }
            size_t eq = line.find('=');
            if (eq == string::npos) continue;
            string key = toLower(trim(line.substr(0, eq)));
            string val = trim(line.substr(eq + 1));

            if      (key == "control_port")   controlPort = configurationInteger(val);
            else if (key == "udp_port_start") udpPortStart = configurationInteger(val);
            else if (key == "udp_port_end")   udpPortEnd = configurationInteger(val);
            else if (key == "hook_port")      hookPort = configurationInteger(val);
            else if (key == "ui_port")        uiPort = configurationInteger(val);
            else if (key == "idle_timeout")   idleTimeoutSec = configurationInteger(val);
            else if (key == "health_interval") healthIntervalSec = configurationInteger(val);
            else if (key == "close_action") closeAction = val;
            else if (key == "tray_notifications") trayNotifications = configurationInteger(val);
            else if (key == "advertise_ip")   advertiseIp = val;
            else if (key == "phone_address_mode") phoneAddressMode = (val == "dhcp_local" || val == "dhcp_remote") ? "dhcp" : val;
            else if (key == "phone_mac") phoneMac = val;
            else if (key == "phone_ip")       phoneIp = val;
            else if (key == "phone_port")     phonePort = configurationInteger(val);
            else if (key == "phone_user")     phoneUser = val;
            else if (key == "phone_pass")     phonePass = val;
            else if (key == "phone_pass_dpapi") secretError = !unprotectLocalSecret(val, phonePass);
            else if (key == "record_dir")     recordDir = val;
            else if (key == "record_folder_mode") recordFolderMode = val;
            else if (key == "record_folder_pattern") recordFolderPattern = val;
            else if (key == "ledger_file")    ledgerFile = val;
            else if (key == "directory_file") directoryFile = val;
            else extra[key] = val;
        }
    }

    /// Return stable field names for invalid settings without including credentials.
    std::vector<string> errors() const {
        std::vector<string> result;
        if (phoneAddressMode != "fixed" && phoneAddressMode != "dhcp") result.push_back("phone_address_mode");
        if (!validIpv4(phoneIp)) result.push_back("phone_ip");
        if (phoneAddressMode == "dhcp" && normalizePhoneMac(phoneMac).empty()) result.push_back("phone_mac");
        if (phoneIp.find_first_of("\r\n") != string::npos) result.push_back("phone_ip");
        if (phoneMac.find_first_of("\r\n") != string::npos) result.push_back("phone_mac");
        if (!validIpv4(advertiseIp)) result.push_back("advertise_ip");
        if (phonePort < 1 || phonePort > 65535) result.push_back("phone_port");
        if (controlPort < 1 || controlPort > 65535) result.push_back("control_port");
        if (phoneAddressMode == "dhcp" && hookPort == 0) result.push_back("hook_port");
        if (hookPort < 0 || hookPort > 65535 || hookPort == controlPort) result.push_back("hook_port");
        if (udpPortStart < 1 || udpPortEnd > 65535 || udpPortEnd < udpPortStart || udpPortEnd - udpPortStart > 1000)
            result.push_back("udp_port_range");
        if (healthIntervalSec != 0 && (healthIntervalSec < 30 || healthIntervalSec > 3600)) result.push_back("health_interval");
        if (trayNotifications != 0 && trayNotifications != 1) result.push_back("tray_notifications");
        if (closeAction != "tray" && closeAction != "minimize" && closeAction != "exit") result.push_back("close_action");
        if (idleTimeoutSec < 2 || idleTimeoutSec > 120) result.push_back("idle_timeout");
        if (recordDir.empty() || recordDir.find_first_of("\r\n") != string::npos) result.push_back("record_dir");
        string recordingFolder;
        if (recordFolderMode != "flat" && recordFolderMode != "daily" && recordFolderMode != "weekly" && recordFolderMode != "custom") result.push_back("record_folder_mode");
        else if (!recordingSubfolder(recordFolderMode, recordFolderPattern, recordingLocalTime(), recordingFolder)) result.push_back("record_folder_pattern");
        if (recordFolderPattern.find_first_of("\r\n") != string::npos) result.push_back("record_folder_pattern");
        if (phoneUser.empty() || phoneUser.find_first_of("\r\n") != string::npos) result.push_back("phone_user");
        if (secretError) result.push_back("phone_pass_dpapi");
        if (formatError) result.push_back("flat_config_required");
        return result;
    }
};


static Config g_cfg;

/// Absolute selected config path; settings save back to this exact file.
static wstring g_configPath = L"config.ini";

#include "phone_address.hpp"

// GUI 回调（由 WinMain 注入，避免录音线程直接依赖界面代码）
typedef void (*RecordStartFn)(const string& name, const string& number);
typedef void (*RecordStopFn)();
static RecordStartFn g_uiRecordStart = NULL;
static RecordStopFn  g_uiRecordStop  = NULL;

// ===========================================================================
#include "json_text.hpp"
#include "recording_titles.hpp"

// 号码 -> 姓名
// ===========================================================================

class Directory {
public:
    /// Load user contacts and decode standard JSON Unicode escapes without changing source files.
    void reload() {
        std::lock_guard<std::mutex> lock(mutex_);
        names_.clear();

        // 优先读 contacts.txt（number=name，可由用户手工维护）
        std::ifstream txt(utf8ToWide("contacts.txt").c_str());
        if (txt) {
            string line;
            bool firstLine = true;
            while (std::getline(txt, line)) {
                if (firstLine) {
                    firstLine = false;
                    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                        (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                        line = line.substr(3);  // 去掉记事本写入的 BOM
                }
                line = trim(line);
                if (line.empty() || line[0] == '#') continue;
                size_t eq = line.find('=');
                if (eq == string::npos) continue;
                names_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
        }

        // 再读 Python 版同步出来的 directory.json，补它没有的
        std::ifstream js(utf8ToWide(g_cfg.directoryFile).c_str());
        if (js) {
            std::stringstream ss;
            ss << js.rdbuf();
            string body = ss.str();
            size_t pos = 0;
            while ((pos = body.find('"', pos)) != string::npos) {
                string key;
                if (!readJsonString(body, pos, key)) break;
                const size_t colon = body.find_first_not_of(" \t\r\n", pos);
                if (colon == string::npos || body[colon] != ':') continue;
                size_t valueAt = body.find_first_not_of(" \t\r\n", colon + 1);
                if (valueAt == string::npos || body[valueAt] != '"') continue;
                string value;
                if (!readJsonString(body, valueAt, value)) break;
                pos = valueAt;
                bool digits = !key.empty() && key.size() < 24;
                for (unsigned char byte : key) if (!isdigit(byte) && byte != '+') digits = false;
                if (digits && !names_.count(key)) names_[key] = value;
            }
        }
        LOGI("号码目录已载入 %d 个号码", (int)names_.size());
    }

    /// Resolve an exact number without placing contact details in diagnostic output.
    string lookup(const string& number) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = names_.find(number);
        return it == names_.end() ? string() : it->second;
    }

    void add(const string& number, const string& name) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            names_[number] = name;
        }

        const char* path = "contacts.txt";
        bool needNewline = false;
        if (fileExists(path)) {
            std::ifstream check(path, std::ios::binary);
            if (check) {
                check.seekg(-1, std::ios::end);
                char last = 0;
                if (check.get(last)) needNewline = (last != '\n');
            }
        }

        std::ofstream out(path, std::ios::app);
        if (out) {
            if (needNewline) out << "\n";
            out << number << "=" << name << "\n";
        }
    }

private:
    std::map<string, string> names_;
    std::mutex mutex_;
};

static Directory g_directory;

// ===========================================================================
// 录音会话
// ===========================================================================

/// Preserve UTF-8 caller display names while excluding Windows filename metacharacters.
static string safeFileName(const string& text) {
    string out;
    for (char c : text) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20)
            continue;
        if (c == ' ') { out += '_'; continue; }
        out += c;
    }
    if (out.size() > 30) {
        size_t length = 30;
        while (length > 0 && (static_cast<unsigned char>(out[length]) & 0xc0) == 0x80) --length;
        out.resize(length);
    }
    return out;
}

#include "recording_core.hpp"

// ===========================================================================
// MD5（用于话机登录 challenge）
// ===========================================================================

class Md5 {
public:
    static string hex(const string& input) {
        HCRYPTPROV prov = 0;
        HCRYPTHASH hash = 0;
        if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            return "";
        }
        if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
            CryptReleaseContext(prov, 0);
            return "";
        }
        CryptHashData(hash, (const BYTE*)input.data(), (DWORD)input.size(), 0);

        BYTE digest[16];
        DWORD len = sizeof(digest);
        CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0);

        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);

        static const char* hexDigits = "0123456789abcdef";
        string out;
        out.reserve(32);
        for (int i = 0; i < 16; i++) {
            out += hexDigits[digest[i] >> 4];
            out += hexDigits[digest[i] & 0x0F];
        }
        return out;
    }
};

// ===========================================================================
// 极简 HTTP 客户端（只够跟话机说话）
// ===========================================================================

#include "compatibility.hpp"
#include "phone_setup.hpp"
#include "phone_client.hpp"

static PhoneClient g_phone;

// ===========================================================================
// 极简 HTTP 服务器
// ===========================================================================

struct HttpRequest {
    string method;
    string path;
    std::map<string, string> query;
    std::map<string, string> headers;
    string body;
    /// Actual TCP peer, never a client-supplied forwarding header.
    string sourceIp;
};

struct HttpResponse {
    int status = 200;
    string contentType = "text/plain; charset=utf-8";
    string body;
    /// Event work begins after the complete HTTP response is sent and its socket is closed.
    std::function<void()> afterSend;
};

static string urlDecode(const string& text) {
    string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '+') { out += ' '; continue; }
        if (text[i] == '%' && i + 2 < text.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out += (char)((hexVal(text[i + 1]) << 4) | hexVal(text[i + 2]));
            i += 2;
            continue;
        }
        out += text[i];
    }
    return out;
}

static std::map<string, string> parseQuery(const string& text) {
    std::map<string, string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t amp = text.find('&', start);
        string part = text.substr(start, amp == string::npos ? string::npos : amp - start);
        size_t eq = part.find('=');
        if (eq != string::npos)
            out[part.substr(0, eq)] = urlDecode(part.substr(eq + 1));
        if (amp == string::npos) break;
        start = amp + 1;
    }
    return out;
}

#include "event_server.hpp"

// ===========================================================================
// 录音文件扫描
// ===========================================================================

struct CallItem {
    string file;
    string date;
    string clock;
    string direction;
    string local;
    string remote;
    string name;
    /// User-defined title, stored separately from the original call identity.
    string title;
    int    duration = 0;
    long long size = 0;
    /// Signal and provenance values from a completed quality sidecar.
    string quality = "unknown", source = "historical_unverified";
};

/// Read a bounded quoted CSV record, including names containing commas, quotes or newlines.
static bool readLedgerRow(std::istream& input, std::vector<string>& fields) {
    fields.clear(); string value; bool quoted = false; size_t bytes = 0; char byte;
    while (input.get(byte)) {
        if (++bytes > 65536) return false;
        if (quoted) {
            if (byte == '"') { if (input.peek() == '"') { input.get(byte); value += '"'; } else quoted = false; }
            else value += byte;
        } else if (byte == '"' && value.empty()) quoted = true;
        else if (byte == ',') { fields.push_back(value); value.clear(); if (fields.size() > 32) return false; }
        else if (byte == '\n') { fields.push_back(value); return true; }
        else if (byte != '\r') value += byte;
    }
    if (!value.empty() || !fields.empty()) { fields.push_back(value); return !quoted; }
    return false;
}

/// Build recording rows from authoritative ledger/sidecar metadata, with legacy filename fallback.
static std::vector<CallItem> scanRecordings() {
    std::vector<CallItem> items;
    std::map<string, std::vector<string>> ledger;
    std::ifstream records(utf8ToWide(g_cfg.ledgerFile), std::ios::binary);
    std::vector<string> record;
    for (int rows = 0; rows < 100000 && readLedgerRow(records, record); ++rows)
        if (record.size() == 9 && record[7].size() >= 4 && record[7].substr(record[7].size() - 4) == ".wav") ledger[record[7]] = record;

    for (const auto& file : recordingFiles(g_cfg.recordDir)) {
        CallItem item;
        item.file = file.relative;
        item.size = file.size;
        item.duration = (int)((item.size - 44) / 16000);
        if (item.duration < 0) item.duration = 0;

        string stem = item.file.substr(item.file.find_last_of("\\/") == string::npos ? 0 : item.file.find_last_of("\\/") + 1);
        size_t dot = stem.rfind('.');
        if (dot != string::npos) stem = stem.substr(0, dot);

        std::vector<string> parts;
        size_t start = 0;
        while (start <= stem.size()) {
            size_t us = stem.find('_', start);
            parts.push_back(stem.substr(start, us == string::npos ? string::npos : us - start));
            if (us == string::npos) break;
            start = us + 1;
        }

        if (parts.size() >= 4) {
            const string& stamp = parts[0];
            if (stamp.size() >= 15) {
                item.date = stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2);
                item.clock = stamp.substr(9, 2) + ":" + stamp.substr(11, 2) + ":" + stamp.substr(13, 2);
            }
            item.local = parts[1];
            item.remote = parts[2];
            item.direction = parts[3];
            size_t nameEnd = parts.size();
            // v2 appends a diagnostic fingerprint even when the phone supplies no name.
            // It is technical identity and must never appear as the contact's display name.
            if (nameEnd > 4 && std::regex_match(parts.back(), std::regex("s-[0-9a-f]{4}-[0-9a-f]{4}"))) --nameEnd;
            for (size_t part = 4; part < nameEnd; ++part) { if (!item.name.empty()) item.name += '_'; item.name += parts[part]; }
        }

        const auto authoritative = ledger.find(item.file);
        if (authoritative != ledger.end()) { const auto& fields = authoritative->second; item.direction = fields[3]; item.local = fields[4]; item.remote = fields[5]; item.name = fields[6]; }
        if (item.name.empty()) item.name = g_directory.lookup(item.remote);
        std::ifstream quality(utf8ToWide(g_cfg.recordDir + "\\" + item.file + ".quality.json"), std::ios::binary);
        char metadata[4097]{};
        quality.read(metadata, 4096);
        const string content(metadata, static_cast<size_t>(quality.gcount()));
        const string signal = jsonField(content, "quality");
        const string source = jsonField(content, "source");
        const string displayName = jsonField(content, "display_name");
        if (!displayName.empty()) item.name = displayName;
        if (!signal.empty()) item.quality = signal;
        if (!source.empty()) item.source = source;
        const auto title = readRecordingTitle(utf8ToWide(g_cfg.recordDir + "\\" + item.file));
        if (!title.error) item.title = wideToUtf8(title.value);
        else LOGW("CALL_TITLE_READ_FAILED file_id=%s win32=%lu", sessionFingerprint(item.file).c_str(), title.error);
        items.push_back(item);
    }

    std::sort(items.begin(), items.end(), [](const CallItem& a, const CallItem& b) {
        if (a.date != b.date) return a.date > b.date;
        if (a.clock != b.clock) return a.clock > b.clock;
        return a.file > b.file;  // 文件名含时间戳，倒序即最新在前
    });
    return items;
}

static string durationText(const CallItem& item) {
    if (item.direction == "missed") return "未接";
    if (item.duration <= 0) return "-";
    char buf[32];
    int minutes = item.duration / 60;
    int seconds = item.duration % 60;
    if (minutes >= 60)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", minutes / 60, minutes % 60, seconds);
    else
        snprintf(buf, sizeof(buf), "%d:%02d", minutes, seconds);
    return buf;
}

/// Reveal a root-relative recording, including its date subfolder.
static void revealFile(const string& fileName) {
    string path = g_cfg.recordDir + "\\" + fileName;
    wstring cmdline = L"explorer.exe /select,\"" + utf8ToWide(path) + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ===========================================================================
// 管理界面
// ===========================================================================

#include "application.hpp"
#include "desktop_ui.hpp"
