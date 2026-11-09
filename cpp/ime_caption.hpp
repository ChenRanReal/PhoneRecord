#pragma once
// Included only from speech_service.hpp after SpeechService is declared.
// Live captions can hand 8 kHz call audio to one installed input method.
// The support table is the only acceptance list; display names are never used as a capability check.

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <msctf.h>
#include <netlistmgr.h>
#include <uiautomation.h>
#include <propvarutil.h>
#include <shellapi.h>
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <MemoryBuffer.h>
#include <cmath>

#pragma comment(lib, "windowsapp.lib")

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "propsys.lib")

/// Bound for "audio is playing but no new input-method text".
constexpr unsigned kImeTextBoundMs = 15000;
/// Silence appended after hangup so the input method can finish its last phrase.
constexpr unsigned kImeTailMs = 800;
/// 8 kHz mono PCM retained for the caption player. Two seconds; older samples are dropped.
constexpr size_t kImeQueueSamples = 8000 * 2;

/// One installed text service as reported by Windows, plus the support-table result.
struct LiveImeEntry {
    /// System display name. Not translated and not used to decide support.
    wstring name;
    /// Canonical GUID strings, including braces.
    string tip, profile;
    /// ASCII model id from the support table, empty when this build cannot drive it.
    string model;
    bool supported = false;
};

/// Active render endpoint. Selecting one here does not change the Windows default device.
struct LivePlaybackDevice { wstring id, name; };

/// Inputs for the per-call engine decision. Network never overwrites the user's saved choice.
struct LiveRouteInput { bool wantIme = false, supported = false, network = false, playbackDevice = false; };
/// startIme is the only path that may open the virtual playback endpoint.
struct LiveRoute { bool startIme = false; const char* phase = "sense"; };

/// Decide one call. Unsupported entries and a missing link at the start stay off the input-method path.
inline LiveRoute routeLiveCaption(const LiveRouteInput& in) {
    if (!in.wantIme) return {};
    if (!in.supported) return {false, "ime_unsupported"};
    if (!in.network) return {};
    if (!in.playbackDevice) return {false, "ime_no_device"};
    return {true, "ime_wait"};
}

/// True only after real samples have reached the playback endpoint and the bound has elapsed.
inline bool imeTextTimedOut(bool audioPlayed, unsigned elapsedMs, unsigned boundMs = kImeTextBoundMs) {
    return audioPlayed && elapsedMs >= boundMs;
}

/// Compare GUID text without treating a product name as evidence of support.
inline string imeGuidKey(string value) {
    for (char& ch : value) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return value;
}

/// Support table. Adding an input method means adding one row; enumeration does not change.
struct ImeSupport { const char* tip; const char* profile; const char* model; };
inline const ImeSupport* imeSupportRows(size_t& count) {
    static const ImeSupport rows[] = {
        {"{86598fb9-66a2-463e-b9c2-aeb906d477ad}", "{607fdf85-fcc8-4dbd-a365-41296f980c9c}", "wetype"},
        {"{9d2b2e2b-3c93-4d2f-9d35-6eeb85f0d2b0}", "{2b4d4b3a-4d4f-4c0a-8e66-7f771a2b9c10}", "doubao"},
        {"{b722b5d7-0c4c-4933-a7b9-df8c91f2c643}", "{0c7479af-f27f-488c-a46b-5bda6bf43e50}", "iflytek"},
        {"{e7ea138e-69f8-11d7-a6ea-00065b844310}", "{e7ea138f-69f8-11d7-a6ea-00065b844311}", "sogou"},
        {"{dcbd6fa8-032f-11d3-b5b1-00c04fc324a1}", "{6a114e62-e11b-447f-9a58-2d354f5c9204}", "windows"},
    };
    count = sizeof(rows) / sizeof(rows[0]); return rows;
}
inline const char* supportedImeModel(const string& tip, const string& profile) {
    const string tipKey = imeGuidKey(tip), profileKey = imeGuidKey(profile);
    size_t count = 0; const ImeSupport* rows = imeSupportRows(count);
    for (size_t index = 0; index < count; ++index) if (tipKey == rows[index].tip && profileKey == rows[index].profile) return rows[index].model;
    return nullptr;
}
inline bool imeResultModel(const string& model) {
    size_t count = 0; const ImeSupport* rows = imeSupportRows(count);
    for (size_t index = 0; index < count; ++index) if (model == rows[index].model) return true;
    return false;
}

/// Format a decimal duration without using the process locale.
inline string imeDecimal(double value) {
    if (value < 0) value = 0;
    int whole = static_cast<int>(value);
    int frac = static_cast<int>(std::llround((value - whole) * 100.0));
    if (frac >= 100) { ++whole; frac -= 100; }
    char buffer[32]; snprintf(buffer, sizeof(buffer), "%d.%02d", whole, frac); return buffer;
}

/// Convert a COM GUID to the braced string stored in preferences.
inline string guidString(const GUID& guid) {
    wchar_t text[64]{}; StringFromGUID2(guid, text, 64); return wideToUtf8(text);
}

/// Bounded 8 kHz queue shared by the recording thread and the playback thread.
class ImePcmQueue {
public:
    /// Copy samples, discarding the oldest when the two-second capacity is exceeded. Does not wait.
    void pushDrop(const int16_t* src, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        append(src, count, true);
    }
    /// Copy only when the whole block fits. The file feeder uses this so a test recording is not discarded.
    bool pushFit(const int16_t* src, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count > kImeQueueSamples || size_ + count > kImeQueueSamples) return false;
        append(src, count, false); return true;
    }
    /// Copy the next samples in queue order.
    size_t peek(int16_t* dst, size_t count) const {
        std::lock_guard<std::mutex> lock(mutex_);
        count = std::min(count, size_);
        for (size_t index = 0; index < count; ++index) dst[index] = data_[(head_ + index) % kImeQueueSamples];
        return count;
    }
    /// Remove samples already accepted by the resampler.
    void consume(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        count = std::min(count, size_);
        head_ = (head_ + count) % kImeQueueSamples; size_ -= count;
    }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return size_; }
private:
    void append(const int16_t* src, size_t count, bool drop) {
        if (!src || count == 0) return;
        if (drop && count >= kImeQueueSamples) { src += count - kImeQueueSamples; count = kImeQueueSamples; head_ = 0; size_ = 0; }
        else if (drop && size_ + count > kImeQueueSamples) {
            const size_t extra = size_ + count - kImeQueueSamples;
            head_ = (head_ + extra) % kImeQueueSamples; size_ -= extra;
        }
        for (size_t index = 0; index < count; ++index) data_[(head_ + size_ + index) % kImeQueueSamples] = src[index];
        size_ += count;
    }
    mutable std::mutex mutex_;
    int16_t data_[kImeQueueSamples]{};
    size_t head_ = 0, size_ = 0;
};

/// Device-clock resampler. It emits only the requested frame count and fills gaps with silence.
class ImeResampler {
public:
    /// Render exactly `frames` device frames from the 8 kHz queue. Extra queued audio stays queued.
    void render(ImePcmQueue& queue, size_t frames, int deviceRate, int channels, float* destination, size_t* silenceFrames) {
        if (deviceRate <= 0 || channels <= 0) return;
        const double step = 8000.0 / deviceRate;
        size_t needed = static_cast<size_t>(std::ceil(origin_ + frames * step)) + 2;
        if (pending_.size() < needed) {
            const size_t ask = needed - pending_.size();
            std::vector<int16_t> more(ask);
            const size_t got = queue.peek(more.data(), ask);
            queue.consume(got); pending_.insert(pending_.end(), more.begin(), more.begin() + static_cast<std::ptrdiff_t>(got));
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            float sample = 0;
            if (origin_ + 1.0 >= static_cast<double>(pending_.size())) {
                if (silenceFrames) ++*silenceFrames;
            } else {
                const size_t left = static_cast<size_t>(origin_);
                const double fraction = origin_ - static_cast<double>(left);
                const float first = pending_[left] / 32768.0f, second = pending_[left + 1] / 32768.0f;
                sample = static_cast<float>(first * (1.0 - fraction) + second * fraction);
                origin_ += step;
            }
            for (int channel = 0; channel < channels; ++channel) destination[frame * static_cast<size_t>(channels) + channel] = sample;
        }
        const size_t drop = static_cast<size_t>(origin_);
        if (drop > 0 && drop <= pending_.size()) { pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop)); origin_ -= static_cast<double>(drop); }
    }
private:
    std::vector<int16_t> pending_;
    double origin_ = 0;
};

/// Publish the single live sidecar. model is the ASCII input-method id. A SenseVoice or Qwen sidecar is left untouched.
inline bool publishImeTranscript(const wstring& wav, const std::vector<std::pair<double, string>>& segments, double duration, bool complete, const string& model = "wetype", const string& original = "", const string& polished = "", bool replace = false) {
    if (wav.size() < 5 || !imeResultModel(model)) return false;
    const wstring jsonPath = wav + L".transcript.json";
    const string existing = SpeechService::read(jsonPath, 1024 * 1024);
    const string existingModel = jsonField(existing, "model");
    if (!replace && !existingModel.empty() && existingModel != model) return false;
    string text;
    string array = "[";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index) array += ",";
        if (!text.empty() && !segments[index].second.empty()) text += "\n";
        text += segments[index].second;
        const double start = index ? segments[index - 1].first : 0;
        array += "{\"start\":" + imeDecimal(start) + ",\"end\":" + imeDecimal(segments[index].first) + ",\"text\":" + jsonQuoted(segments[index].second) + "}";
    }
    array += "]";
    if (!original.empty() || !polished.empty()) {
        text.clear();
        if (!original.empty()) text += string("原文\n") + original;
        if (!polished.empty()) { if (!text.empty()) text += "\n"; text += string("整理后\n") + polished; }
    }
    const string json = "{\"schema\":1,\"model\":" + jsonQuoted(model) + ",\"engine\":" + jsonQuoted(model + "-ime") + ",\"language\":\"\",\"mode\":\"live\",\"sourceSha256\":\"\",\"duration\":" + imeDecimal(duration) + ",\"status\":\"" + string(complete ? "complete" : "running") + "\",\"segments\":" + array + ",\"text\":" + jsonQuoted(text) + ",\"original\":" + jsonQuoted(original) + ",\"polished\":" + jsonQuoted(polished) + "}";
    string plain;
    for (char ch : text) {
        if (ch == '\r') { plain += "\r\n"; }
        else if (ch == '\n') plain += "\r\n";
        else plain += ch;
    }
    wstring live = wav;
    if (live.size() >= 4 && _wcsicmp(live.c_str() + live.size() - 4, L".wav") == 0) live.replace(live.size() - 4, 4, L".live.txt");
    else live += L".live.txt";
    const string bom = string("\xEF\xBB\xBF") + plain;
    return SpeechService::write(wav + L".transcript.txt", text) && SpeechService::write(live, bom) && SpeechService::write(jsonPath, json);
}

/// Initialize COM for one worker thread and remember whether this call must uninitialize it.
inline HRESULT imeThreadCom(bool& uninit) {
    const HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    uninit = hr == S_OK || hr == S_FALSE; return hr;
}

inline bool systemHasInternet() {
    bool uninit = false; imeThreadCom(uninit);
    INetworkListManager* manager = nullptr;
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    const HRESULT created = CoCreateInstance(CLSID_NetworkListManager, NULL, CLSCTX_ALL, IID_PPV_ARGS(&manager));
    HRESULT result = created;
    if (SUCCEEDED(created)) result = manager->GetConnectivity(&connectivity);
    if (manager) manager->Release();
    if (uninit) CoUninitialize();
    return SUCCEEDED(result) && (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
}

inline std::vector<LivePlaybackDevice> enumeratePlaybackDevices() {
    std::vector<LivePlaybackDevice> found;
    bool uninit = false; imeThreadCom(uninit);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0; collection->GetCount(&count);
            for (UINT index = 0; index < count; ++index) {
                IMMDevice* device = nullptr; if (FAILED(collection->Item(index, &device))) continue;
                LPWSTR id = nullptr; IPropertyStore* store = nullptr; LivePlaybackDevice row;
                if (SUCCEEDED(device->GetId(&id)) && id) { row.id = id; CoTaskMemFree(id); }
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
                    PROPVARIANT value; PropVariantInit(&value);
                    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) row.name = value.pwszVal;
                    PropVariantClear(&value); store->Release();
                }
                if (!row.id.empty() && !row.name.empty()) found.push_back(std::move(row));
                device->Release();
            }
            collection->Release();
        }
        enumerator->Release();
    }
    if (uninit) CoUninitialize();
    return found;
}

inline wstring imeShortName(wstring name) {
    const wchar_t* marks[] = {L"中文(简体) - ", L"中文（简体） - ", L"中文(简体)－", L"中文（简体）－", L"中文(繁体) - ", L"中文（繁體） - "};
    for (const wchar_t* mark : marks) {
        const size_t length = wcslen(mark);
        if (name.size() >= length && name.compare(0, length, mark) == 0) return name.substr(length);
    }
    return name;
}

inline wstring findVirtualCableId(const wstring& preferred = L"") {
    const auto devices = enumeratePlaybackDevices();
    if (!preferred.empty()) for (const auto& device : devices) if (device.id == preferred) return device.id;
    for (const auto& device : devices) {
        if (device.name.find(L"16") != wstring::npos) continue;
        if (device.name.find(L"CABLE Input") != wstring::npos) return device.id;
    }
    return L"";
}

/// List enabled text services. Installed-but-disabled profiles are omitted; unsupported ones are still returned.
inline std::vector<LiveImeEntry> enumerateEnabledImes() {
    std::vector<LiveImeEntry> found;
    bool uninit = false; imeThreadCom(uninit);
    ITfInputProcessorProfiles* profiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles)))) {
        ITfInputProcessorProfileMgr* manager = nullptr;
        if (SUCCEEDED(profiles->QueryInterface(IID_PPV_ARGS(&manager)))) {
            IEnumTfInputProcessorProfiles* enumerator = nullptr;
            if (SUCCEEDED(manager->EnumProfiles(0, &enumerator)) && enumerator) {
                TF_INPUTPROCESSORPROFILE profile{};
                while (enumerator->Next(1, &profile, NULL) == S_OK) {
                    if (profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR || (profile.dwFlags & TF_IPP_FLAG_ENABLED) == 0) continue;
                    BSTR description = nullptr;
                    profiles->GetLanguageProfileDescription(profile.clsid, profile.langid, profile.guidProfile, &description);
                    LiveImeEntry row;
                    row.name = description ? description : L"";
                    if (description) SysFreeString(description);
                    row.tip = guidString(profile.clsid); row.profile = guidString(profile.guidProfile);
                    if (const char* model = supportedImeModel(row.tip, row.profile)) { row.model = model; row.supported = true; }
                    if (row.name.empty()) row.name = utf8ToWide(row.tip);
                    row.name = imeShortName(row.name);
                    found.push_back(std::move(row));
                }
                enumerator->Release();
            }
            manager->Release();
        }
        profiles->Release();
    }
    if (uninit) CoUninitialize();
    size_t supportCount = 0; const ImeSupport* support = imeSupportRows(supportCount);
    for (size_t index = 0; index < supportCount; ++index) {
        bool present = false;
        for (const auto& item : found) if (item.model == support[index].model) present = true;
        if (present) continue;
        const wstring root = L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + utf8ToWide(support[index].tip) + L"\\LanguageProfile";
        HKEY languages = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root.c_str(), 0, KEY_READ, &languages) != ERROR_SUCCESS) continue;
        wchar_t language[64]; wstring description;
        for (DWORD lang = 0; description.empty() && RegEnumKeyW(languages, lang, language, 64) == ERROR_SUCCESS; ++lang) {
            HKEY profileKey = NULL;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (root + L"\\" + language + L"\\" + utf8ToWide(support[index].profile)).c_str(), 0, KEY_READ, &profileKey) != ERROR_SUCCESS) continue;
            wchar_t text[256]{}; DWORD bytes = sizeof(text), kind = 0;
            if (RegQueryValueExW(profileKey, L"Description", NULL, &kind, reinterpret_cast<BYTE*>(text), &bytes) == ERROR_SUCCESS && kind == REG_SZ) description = text;
            RegCloseKey(profileKey);
        }
        RegCloseKey(languages);
        if (description.empty()) continue;
        LiveImeEntry row; row.tip = support[index].tip; row.profile = support[index].profile; row.model = support[index].model; row.supported = true; row.name = imeShortName(description);
        found.push_back(std::move(row));
    }
    return found;
}

/// Recognize the pixels of one voice panel. Flutter does not expose this text through UI Automation.
inline wstring ocrWindowText(HWND window) {
    RECT rect{}; if (!GetWindowRect(window, &rect)) return L"";
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width < 40 || height < 40 || width > 2000 || height > 1600) return L"";
    HDC dc = GetWindowDC(window); if (!dc) return L"";
    HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height); HGDIOBJ old = SelectObject(memory, bitmap);
    // The panel is topmost, so the composited desktop pixels are the reliable source: Flutter's
    // hardware surface renders as a blank bitmap through PrintWindow.
    HDC screen = GetDC(NULL);
    if (screen) { BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT); ReleaseDC(NULL, screen); }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    size_t ink = 0;
    for (size_t index = 0; index < pixels.size(); index += 4) if (pixels[index] > 32 || pixels[index + 1] > 32 || pixels[index + 2] > 32) { if (++ink > 200) break; }
    if (ink <= 200) { PrintWindow(window, memory, 2); GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS); }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
    try {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        SoftwareBitmap image(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
        {
            auto locked = image.LockBuffer(BitmapBufferAccessMode::ReadWrite);
            auto access = locked.CreateReference().as<Windows::Foundation::IMemoryBufferByteAccess>();
            uint8_t* data = nullptr; uint32_t capacity = 0;
            if (FAILED(access->GetBuffer(&data, &capacity)) || !data) return L"";
            const auto plane = locked.GetPlaneDescription(0);
            for (int row = 0; row < height; ++row) {
                const size_t source = static_cast<size_t>(row) * width * 4;
                const size_t target = static_cast<size_t>(plane.StartIndex) + static_cast<size_t>(row) * plane.Stride;
                if (source + width * 4 > pixels.size() || target + width * 4 > capacity) return L"";
                memcpy(data + target, pixels.data() + source, static_cast<size_t>(width) * 4);
            }
        }
        auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(L"zh-Hans"));
        if (!engine) return L"";
        const auto recognized = engine.RecognizeAsync(image).get();
        wstring text = recognized.Text().c_str();
        wstring kept; size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find(L'\n', start);
            wstring line = text.substr(start, end == wstring::npos ? wstring::npos : end - start);
            while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
            while (!line.empty() && iswspace(line.back())) line.pop_back();
            const bool chrome = line == L"语音输入" || line.find(L"Ctrl") != wstring::npos || line.find(L"Esc") != wstring::npos || line.find(L"麦克风") != wstring::npos || line.find(L"结束") != wstring::npos;
            if (!line.empty() && !chrome) { if (!kept.empty()) kept += L"\n"; kept += line; }
            if (end == wstring::npos) break;
            start = end + 1;
        }
        return kept;
    } catch (...) { return L""; }
}

/// A voice panel belongs to one supported input method. The window class is never the only condition.
inline bool imeVoiceProcess(const string& model, const wchar_t* exe, const wstring& title, const wchar_t* klass = L"", int width = 0, int height = 0) {
    const bool voiceTitle = title == L"语音输入" || title == L"Voice typing" || title.find(L"语音") != wstring::npos || title.find(L"語音") != wstring::npos;
    const bool settings = title.find(L"设置") != wstring::npos || title.find(L"設定") != wstring::npos || title.find(L"登录") != wstring::npos || title.find(L"登入") != wstring::npos;
    if (settings) return false;
    if (model == "wetype") {
        const bool sameExe = _wcsicmp(exe, L"wetype_renderer.exe") == 0 || _wcsicmp(exe, L"wetype_server.exe") == 0 || _wcsicmp(exe, L"wetype_service.exe") == 0 || _wcsicmp(exe, L"wetype_update.exe") == 0;
        if (!sameExe) return false;
        if (title == L"语音输入") return true;
        // The Flutter voice bar draws its own caption; it is the only panel of that class at this aspect ratio.
        return klass && _wcsicmp(klass, L"wetype.flutter.setting") == 0 && height > 0 && width >= 300 && width <= 900 && width * 10 >= height * 14 && width * 10 <= height * 19;
    }
    if (model == "doubao") return _wcsicmp(exe, L"ImeService.exe") == 0 && (voiceTitle || (klass && _wcsicmp(klass, L"OimeVoiceWaveWindow") == 0));
    if (model == "iflytek") return _wcsicmp(exe, L"iFlyVoice.exe") == 0 || ((_wcsicmp(exe, L"iFlyInput.exe") == 0 || _wcsicmp(exe, L"iFlyPlatform.exe") == 0) && voiceTitle);
    if (model == "sogou") return _wcsicmp(exe, L"sogou_voice_assistant.exe") == 0 || ((_wcsicmp(exe, L"SogouCloud.exe") == 0 || _wcsicmp(exe, L"SOGOUSmartAssistant.exe") == 0) && voiceTitle);
    if (model == "windows") return (_wcsicmp(exe, L"TextInputHost.exe") == 0 && (title == L"语音输入" || title == L"Voice typing")) || _wcsicmp(exe, L"SpeechUX.exe") == 0;
    return false;
}

/// Separate the recognizer's original line from its cleaned-up line when the panel shows both.
inline void splitImePanel(const wstring& text, wstring& original, wstring& polished) {
    original.clear(); polished.clear(); int section = 0; bool sawPolish = false;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L'\n', start);
        wstring line = text.substr(start, end == wstring::npos ? wstring::npos : end - start);
        while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        const bool polish = line == L"整理后" || line == L"整理後" || line == L"智能整理" || line == L"智能润色" || line == L"智能潤色" || line == L"润色结果" || line == L"潤色結果";
        const bool raw = line == L"原文" || line == L"识别结果" || line == L"識別結果";
        if (polish) { section = 1; sawPolish = true; }
        else if (raw) section = 0;
        else if (!line.empty()) {
            wstring& target = section == 1 ? polished : original;
            if (!target.empty()) target += L"\n";
            target += line;
        }
        if (end == wstring::npos) break;
        start = end + 1;
    }
    if (!sawPolish) { original = text; polished.clear(); }
}

/// Window chrome that accessibility exposes for an input method. It is not recognized speech.
inline bool imePanelChrome(const wstring& line) {
    if (line.size() < 2) return true;
    if (line == L"语音输入" || line == L"普通话" || line == L"粤语" || line == L"粵語" || line == L"英语" || line == L"英語" || line == L"English" || line == L"请开始说话" || line == L"請開始說話") return true;
    if (line == L"关闭" || line == L"關閉" || line == L"取消" || line == L"完成" || line == L"设置" || line == L"設定" || line == L"关闭窗口" || line == L"關閉視窗") return true;
    // The voice bar prints its own shortcut hint and microphone advice below the recognized text.
    if (line == L"Ctrl" || line == L"Win" || line == L"Shift" || line == L"Alt" || line == L"Esc" || line == L"Enter" || line == L"结束" || line == L"結束" || line == L"或" || line == L"or") return true;
    if (line == L"结束录制" || line == L"結束錄製" || line == L"完成录制" || line == L"完成錄製" || line == L"重新录制" || line == L"重新錄製" || line == L"正在录音" || line == L"正在錄音") return true;
    if (line.find(L"麦克风音量") == 0 || line.find(L"麥克風音量") == 0 || line.find(L"调高后") != wstring::npos) return true;
    if (line.find(L"Chrome Legacy Window") != wstring::npos) return true;
    if (line.find(L"file://") != wstring::npos || line.find(L"app.asar") != wstring::npos) return true;
    if (line.find(L"主面板") != wstring::npos || line.find(L"输入气泡") != wstring::npos || line.find(L"輸入氣泡") != wstring::npos) return true;
    if (line.find(L"语音悬浮窗") != wstring::npos || line.find(L"語音懸浮窗") != wstring::npos) return true;
    return false;
}

inline wstring imePanelBody(const wstring& text) {
    wstring kept; bool content = false; size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L'\n', start);
        wstring line = text.substr(start, end == wstring::npos ? wstring::npos : end - start);
        while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        const bool header = line == L"原文" || line == L"整理后" || line == L"整理後";
        if (!line.empty() && !imePanelChrome(line)) {
            if (!header) content = true;
            if (!kept.empty()) kept += L"\n";
            kept += line;
        }
        if (end == wstring::npos) break;
        start = end + 1;
    }
    return content ? kept : L"";
}

inline string imePanelUtf8(const string& text) { return wideToUtf8(imePanelBody(utf8ToWide(text))); }

/// Read text from the selected input method's voice panel. The window class is not an acceptance condition.
/// Read text from the selected input method's voice panel. The window class is not an acceptance condition.
inline wstring readImeVoicePanel(const string& model) {
    struct Candidate { HWND window; };
    std::vector<Candidate> windows;
    struct Query { const string* model; std::vector<Candidate>* windows; };
    Query query{&model, &windows};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* query = reinterpret_cast<Query*>(param);
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t title[256]{}, klass[128]{}; GetWindowTextW(window, title, 256); GetClassNameW(window, klass, 128);
        wstring text = title;
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        RECT rect{}; GetWindowRect(window, &rect);
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return TRUE;
        wchar_t path[1024]{}; DWORD size = 1024;
        const BOOL named = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        if (!named) return TRUE;
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        if (!imeVoiceProcess(*query->model, base, text, klass, rect.right - rect.left, rect.bottom - rect.top)) return TRUE;
        query->windows->push_back({window}); return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    if (windows.empty()) return L"";
    bool uninit = false; imeThreadCom(uninit);
    IUIAutomation* automation = nullptr; wstring collected;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        for (const auto& candidate : windows) {
            IUIAutomationElement* root = nullptr;
            if (FAILED(automation->ElementFromHandle(candidate.window, &root)) || !root) continue;
            IUIAutomationTreeWalker* walker = nullptr;
            if (FAILED(automation->get_RawViewWalker(&walker)) || !walker) { root->Release(); continue; }
            std::vector<IUIAutomationElement*> stack{root};
            int seen = 0;
            while (!stack.empty() && seen < 400) {
                IUIAutomationElement* node = stack.back(); stack.pop_back(); ++seen;
                BSTR name = nullptr; node->get_CurrentName(&name);
                if (name && wcscmp(name, L"语音输入") != 0) {
                    wstring line = name;
                    if (line.size() > 1 && collected.find(line) == wstring::npos) { if (!collected.empty()) collected += L"\n"; collected += line; }
                }
                if (name) SysFreeString(name);
                IUIAutomationValuePattern* value = nullptr;
                if (SUCCEEDED(node->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value) {
                    BSTR current = nullptr; value->get_CurrentValue(&current);
                    if (current && *current && wcscmp(current, L"语音输入") != 0) {
                        wstring line = current;
                        if (collected.find(line) == wstring::npos) { if (!collected.empty()) collected += L"\n"; collected += line; }
                    }
                    if (current) SysFreeString(current); value->Release();
                }
                IUIAutomationElement* child = nullptr;
                if (SUCCEEDED(walker->GetFirstChildElement(node, &child)) && child) {
                    stack.push_back(child);
                    while (child) {
                        IUIAutomationElement* next = nullptr;
                        if (FAILED(walker->GetNextSiblingElement(child, &next))) break;
                        if (next) stack.push_back(next);
                        child = next;
                    }
                }
                node->Release();
            }
            walker->Release();
        }
        automation->Release();
    }
    wstring body = imePanelBody(collected);
    if (body.empty()) {
        for (const auto& candidate : windows) {
            const wstring seen = imePanelBody(ocrWindowText(candidate.window));
            if (seen.size() > body.size()) body = seen;
        }
    }
    if (uninit) CoUninitialize();
    return body;
}

struct ImeLiveOwner;
/// Defined with the panel helpers below; declared here so a finished session can end the voice bar.
inline void closeImeVoice(const string& model);

/// File transcription shares one playback owner with the page that started it.
/// Live captions do not publish progress here, so a closed page can still show time and text.
struct ImeFileSession {
    static std::mutex& mutex() { static std::mutex value; return value; }
    static std::shared_ptr<ImeLiveOwner>& owner();
    static std::atomic<bool>& running() { static std::atomic<bool> value{false}; return value; }
    static std::atomic<uint64_t>& playedMs() { static std::atomic<uint64_t> value{0}; return value; }
    static std::atomic<uint64_t>& totalMs() { static std::atomic<uint64_t> value{0}; return value; }
    /// 0 while unknown, 1 after the voice panel is visible, 2 when it never appeared.
    static std::atomic<int>& voiceOpened() { static std::atomic<int> value{0}; return value; }
    static void note(const string& model, const string& original, const string& polished) {
        std::lock_guard<std::mutex> lock(textMutex());
        textModel() = model; textOriginal() = original; textPolished() = polished;
    }
    static bool snapshot(string& model, string& original, string& polished) {
        std::lock_guard<std::mutex> lock(textMutex());
        model = textModel(); original = textOriginal(); polished = textPolished();
        return !original.empty() || !polished.empty();
    }
    static void stop();
private:
    static std::mutex& textMutex() { static std::mutex value; return value; }
    static string& textModel() { static string value; return value; }
    static string& textOriginal() { static string value; return value; }
    static string& textPolished() { static string value; return value; }
};

/// Owns the playback and panel threads for one live recording.
struct ImeLiveOwner : std::enable_shared_from_this<ImeLiveOwner> {
    /// Prepare one call. Threads start only from start(), after the service has published this owner.
    ImeLiveOwner(wstring wav, wstring deviceId, string model, std::function<void(const char*)> fallback, bool filePlayback = false)
        : wav_(std::move(wav)), deviceId_(std::move(deviceId)), model_(std::move(model)), fallback_(std::move(fallback)), filePlayback_(filePlayback) {}
    /// Start playback and the panel reader. Neither thread activates another window.
    void start() {
        if (started_.exchange(true)) return;
        audio_ = std::thread([this] { audioMain(); });
        reader_ = std::thread([this] { readerMain(); });
    }
    void pushDrop(const int16_t* pcm, int count) { if (!stop_.load()) queue_.pushDrop(pcm, count); }
    bool pushFit(const int16_t* pcm, int count) { return !stop_.load() && queue_.pushFit(pcm, count); }
    bool stopped() const { return stop_.load(); }
    void requestStop() { stop_.store(true); drained_.notify_all(); }
    /// Queue the hangup tail, then wait until the device clock has played it or the wait bound elapses.
    void finishTail() {
        std::vector<int16_t> silence(8000 * kImeTailMs / 1000);
        size_t offset = 0;
        const auto deadline = GetTickCount64() + 3000;
        while (offset < silence.size() && GetTickCount64() < deadline && !stop_.load()) {
            const size_t block = std::min<size_t>(160, silence.size() - offset);
            if (!queue_.pushFit(silence.data() + offset, block)) { Sleep(5); continue; }
            offset += block;
        }
        tail_.store(true);
        const auto done = GetTickCount64() + 3000;
        std::unique_lock<std::mutex> lock(waitMutex_);
        drained_.wait_for(lock, std::chrono::milliseconds(done > GetTickCount64() ? done - GetTickCount64() : 1), [&] { return audioDrained_.load() || stop_.load(); });
        lock.unlock();
        const auto visible = GetTickCount64() + 8000;
        do { capturePanel(false); if (!text().empty()) break; Sleep(400); } while (GetTickCount64() < visible);
        capturePanel(true);
        requestStop();
    }
    void join() {
        requestStop();
        if (audio_.joinable()) audio_.join();
        if (reader_.joinable()) reader_.join();
    }
    string text() const { std::lock_guard<std::mutex> lock(textMutex_); return shown_; }
private:
    void fail(const char* phase) {
        if (failed_.exchange(true)) return;
        requestStop();
        if (fallback_) fallback_(phase);
    }
    void remember(const wstring& panel, bool complete) {
        wstring original, polished; splitImePanel(panel, original, polished);
        const string nextOriginal = wideToUtf8(original), nextPolished = wideToUtf8(polished);
        const string incoming = nextPolished.empty() ? nextOriginal : nextOriginal + "\n" + nextPolished;
        std::lock_guard<std::mutex> lock(textMutex_);
        if (incoming.empty() || incoming == shown_) { if (complete) publishLocked(true); return; }
        shown_ = incoming; original_ = nextOriginal; polished_ = nextPolished; lastText_ = GetTickCount64();
        segments_.clear(); segments_.push_back({seconds_, incoming});
        if (filePlayback_) ImeFileSession::note(model_, original_, polished_);
        publishLocked(complete);
    }
    void publishLocked(bool complete) {
        if (segments_.empty() && original_.empty() && polished_.empty()) return;
        publishImeTranscript(wav_, segments_, seconds_, complete, model_, original_, polished_, filePlayback_);
    }
    void capturePanel(bool complete) {
        const wstring panel = readImeVoicePanel(model_);
        if (!panel.empty() || complete) remember(panel, complete);
    }
    void readerMain() {
        bool uninit = false; imeThreadCom(uninit);
        while (!stop_.load()) {
            capturePanel(false);
            if (!filePlayback_ && audioHeard_.load()) {
                const unsigned elapsed = static_cast<unsigned>(GetTickCount64() - lastText_.load());
                if (imeTextTimedOut(true, elapsed) && !failed_.load()) { fail("ime_fallback"); break; }
            }
            Sleep(300);
        }
        if (uninit) CoUninitialize();
    }
    void audioMain() {
        bool uninit = false; imeThreadCom(uninit);
        IMMDeviceEnumerator* enumerator = nullptr; IMMDevice* device = nullptr; IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* format = nullptr; HANDLE event = NULL; bool started = false, released = false;
        auto release = [&] {
            if (released) return; released = true;
            if (started && client) client->Stop();
            if (render) render->Release(); if (client) client->Release(); if (device) device->Release(); if (enumerator) enumerator->Release();
            if (format) CoTaskMemFree(format); if (event) CloseHandle(event);
            audioDrained_.store(true); drained_.notify_all();
        };
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDevice(deviceId_.c_str(), &device)) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&client))) ||
            FAILED(client->GetMixFormat(&format))) { release(); fail("ime_no_device"); if (uninit) CoUninitialize(); return; }
        const auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
        const bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22 && extensible->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT && extensible->SubFormat.Data2 == 0x0000);
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!floating || !event || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format, NULL)) || FAILED(client->SetEventHandle(event)) || FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
            release(); fail("ime_no_device"); if (uninit) CoUninitialize(); return;
        }
        UINT32 buffer = 0; client->GetBufferSize(&buffer);
        if (FAILED(client->Start())) { release(); fail("ime_no_device"); if (uninit) CoUninitialize(); return; }
        started = true; lastText_.store(GetTickCount64());
        while (!stop_.load()) {
            const DWORD wait = WaitForSingleObject(event, 500);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;
            UINT32 padding = 0;
            const HRESULT paddingResult = client->GetCurrentPadding(&padding);
            if (paddingResult == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(paddingResult)) { fail("ime_no_device"); break; }
            const UINT32 frames = buffer > padding ? buffer - padding : 0;
            if (!frames) continue;
            BYTE* data = nullptr; if (FAILED(render->GetBuffer(frames, &data))) { fail("ime_no_device"); break; }
            size_t silence = 0; ImeResampler local = resampler_;
            local.render(queue_, frames, format->nSamplesPerSec, format->nChannels, reinterpret_cast<float*>(data), &silence);
            resampler_ = local;
            if (silence < frames) {
                audioHeard_.store(true);
                seconds_ += static_cast<double>(frames - silence) / format->nSamplesPerSec;
                if (filePlayback_) ImeFileSession::playedMs().store(static_cast<uint64_t>(seconds_ * 1000.0));
            }
            if (FAILED(render->ReleaseBuffer(frames, silence == frames ? AUDCLNT_BUFFERFLAGS_SILENT : 0))) { fail("ime_no_device"); break; }
            if (tail_.load() && queue_.size() == 0 && silence == frames) { audioDrained_.store(true); drained_.notify_all(); }
        }
        release(); if (uninit) CoUninitialize();
    }
    wstring wav_, deviceId_;
    string model_;
    std::function<void(const char*)> fallback_;
    ImePcmQueue queue_;
    ImeResampler resampler_;
    std::thread audio_, reader_;
    std::atomic<bool> started_{false}, stop_{false}, tail_{false}, audioDrained_{false}, audioHeard_{false}, failed_{false};
    std::atomic<ULONGLONG> lastText_{0};
    std::mutex waitMutex_;
    mutable std::mutex textMutex_;
    std::condition_variable drained_;
    string shown_, original_, polished_;
    std::vector<std::pair<double, string>> segments_;
    double seconds_ = 0;
    bool filePlayback_ = false;
};

inline std::shared_ptr<ImeLiveOwner>& ImeFileSession::owner() {
    static std::shared_ptr<ImeLiveOwner> value;
    return value;
}

inline void ImeFileSession::stop() {
    std::shared_ptr<ImeLiveOwner> current;
    { std::lock_guard<std::mutex> lock(mutex()); current = owner(); }
    if (current) current->requestStop();
}

inline void SpeechService::armLive(const wstring& wav, const string& engine, const string& tip, const string& profile, const string& device, bool network, bool deviceOk, std::shared_ptr<ImeLiveOwner>& starting) {
    const bool supported = supportedImeModel(tip, profile) != nullptr;
    const LiveRoute route = routeLiveCaption({engine == "ime", supported, network, deviceOk});
    state_.liveEngine = engine; state_.livePhase = route.phase;
    if (!route.startIme) {
        imeActive_.store(false);
        if (installed("sense")) {
            livePending_ = true; liveRan_ = false;
            if (state_.busy && !installingNow_) { preempt_ = true; ++epoch_; }
        }
        return;
    }
    const char* model = supportedImeModel(tip, profile); if (!model) model = "wetype";
    auto owner = std::make_shared<ImeLiveOwner>(wav, utf8ToWide(device), model, [this](const char* phase) { fallbackFromIme(phase); });
    { std::lock_guard<std::mutex> lock(imeMutex_); imeOwner_ = owner; }
    imeActive_.store(true); starting = std::move(owner);
}

inline void SpeechService::fallbackFromIme(const char* phase) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || imeFellBack_.load()) return;
        imeFellBack_.store(true); state_.livePhase = phase ? phase : "ime_fallback"; ++state_.revision;
        if (state_.recording && installed("sense")) {
            livePending_ = true; liveRan_ = false;
            if (state_.busy && !installingNow_) { preempt_ = true; ++epoch_; }
            launchThread(); wake_.notify_all();
        }
    }
    requestImeStop();
}

inline void SpeechService::requestImeStop() {
    std::shared_ptr<ImeLiveOwner> owner;
    { std::lock_guard<std::mutex> lock(imeMutex_); owner = imeOwner_; }
    if (owner) owner->requestStop();
}

inline void SpeechService::finishIme(const wstring& wav) {
    bool tail = false;
    { std::lock_guard<std::mutex> lock(mutex_); if (state_.liveWav != wav) return; tail = !imeFellBack_.load(); }
    std::shared_ptr<ImeLiveOwner> owner;
    { std::lock_guard<std::mutex> lock(imeMutex_); owner = std::move(imeOwner_); }
    imeActive_.store(false);
    if (!owner) return;
    if (tail) owner->finishTail(); else owner->requestStop();
    owner->join();
    // The bar keeps recording the room after the call; close it so the input method stops listening.
    string tip, profile;
    { std::lock_guard<std::mutex> lock(mutex_); tip = liveImeTip_; profile = liveImeProfile_; }
    if (const char* model = supportedImeModel(tip, profile)) closeImeVoice(model);
}

inline void SpeechService::discardImeOwner() {
    std::shared_ptr<ImeLiveOwner> owner;
    { std::lock_guard<std::mutex> lock(imeMutex_); owner = std::move(imeOwner_); }
    imeActive_.store(false);
    if (!owner) return;
    owner->requestStop(); owner->join();
    string tip, profile;
    { std::lock_guard<std::mutex> lock(mutex_); tip = liveImeTip_; profile = liveImeProfile_; }
    if (const char* model = supportedImeModel(tip, profile)) closeImeVoice(model);
}

inline void SpeechService::livePcm(const int16_t* pcm, int count) {
    if (!pcm || count <= 0 || !imeActive_.load()) return;
    std::shared_ptr<ImeLiveOwner> owner;
    { std::lock_guard<std::mutex> lock(imeMutex_); owner = imeOwner_; }
    if (owner) owner->pushDrop(pcm, count);
}

struct ImeChord { bool ctrl = false, shift = false, alt = false, win = false; WORD altVk = VK_MENU; WORD key = 0; bool used() const { return ctrl || shift || alt || win || key != 0; } };

/// Read a shortcut label such as "Ctrl + Shift + Alt + [". Modifier-only chords have no key.
inline bool imeChordFromLabel(const wstring& label, ImeChord& chord) {
    chord = {};
    size_t start = 0;
    while (start <= label.size()) {
        const size_t plus = label.find(L'+', start);
        wstring token = label.substr(start, plus == wstring::npos ? wstring::npos : plus - start);
        while (!token.empty() && iswspace(token.front())) token.erase(token.begin());
        while (!token.empty() && iswspace(token.back())) token.pop_back();
        if (_wcsicmp(token.c_str(), L"Ctrl") == 0 || token == L"Control") chord.ctrl = true;
        else if (_wcsicmp(token.c_str(), L"Shift") == 0) chord.shift = true;
        else if (_wcsicmp(token.c_str(), L"Alt") == 0) chord.alt = true;
        else if (_wcsicmp(token.c_str(), L"Win") == 0 || _wcsicmp(token.c_str(), L"Windows") == 0) chord.win = true;
        else if (token == L"[") chord.key = VK_OEM_4;
        else if (token == L"]") chord.key = VK_OEM_6;
        else if (token == L"'") chord.key = VK_OEM_7;
        else if (token == L";") chord.key = VK_OEM_1;
        else if (token == L",") chord.key = VK_OEM_COMMA;
        else if (token == L".") chord.key = VK_OEM_PERIOD;
        else if (token == L"/") chord.key = VK_OEM_2;
        else if (token == L"-") chord.key = VK_OEM_MINUS;
        else if (token == L"=") chord.key = VK_OEM_PLUS;
        else if (token == L"`") chord.key = VK_OEM_3;
        else if (token == L"\\") chord.key = VK_OEM_5;
        else if (_wcsicmp(token.c_str(), L"Space") == 0) chord.key = VK_SPACE;
        else if (token.size() == 1 && iswalpha(token[0])) chord.key = static_cast<WORD>(towupper(token[0]));
        else if (token.size() == 1 && iswdigit(token[0])) chord.key = static_cast<WORD>(token[0]);
        if (plus == wstring::npos) break;
        start = plus + 1;
    }
    return chord.used();
}

inline bool imeNoticeVisible(const wchar_t* mark) {
    std::pair<const wchar_t*, bool> query{mark, false};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* query = reinterpret_cast<std::pair<const wchar_t*, bool>*>(param);
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t title[256]{}; GetWindowTextW(window, title, 256);
        if (wcsstr(title, query->first)) { query->second = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.second;
}

/// Locate the visible voice panel of one input method.
inline HWND findImeVoiceWindow(const string& model) {
    struct Query { const string* model; HWND found; };
    Query query{&model, NULL};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* query = reinterpret_cast<Query*>(param);
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t title[256]{}, klass[128]{}; GetWindowTextW(window, title, 256); GetClassNameW(window, klass, 128);
        wstring text = title;
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        RECT rect{}; GetWindowRect(window, &rect);
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return TRUE;
        wchar_t path[1024]{}; DWORD size = 1024;
        const BOOL named = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        if (!named) return TRUE;
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        if (!imeVoiceProcess(*query->model, base, text, klass, rect.right - rect.left, rect.bottom - rect.top)) return TRUE;
        query->found = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.found;
}

/// A voice panel left open by an earlier run is not listening; close it so the shortcut starts a fresh session.
inline void resetImeVoice(const string& model) {
    HWND window = findImeVoiceWindow(model);
    if (!window) return;
    PostMessageW(window, WM_CLOSE, 0, 0);
    const auto deadline = GetTickCount64() + 1500;
    while (findImeVoiceWindow(model) && GetTickCount64() < deadline) Sleep(60);
    // Give the input method a moment to finish tearing the bar down before the shortcut arrives.
    Sleep(450);
}

/// End one voice session without waiting; the panel stops recording as soon as it closes.
inline void closeImeVoice(const string& model) {
    if (!imeResultModel(model)) return;
    if (HWND window = findImeVoiceWindow(model)) PostMessageW(window, WM_CLOSE, 0, 0);
}

inline bool imeVoiceVisible(const string& model) {
    return findImeVoiceWindow(model) != NULL;
}

inline void imeApplyModifier(ImeChord& chord, unsigned vk) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) chord.ctrl = true;
    else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) chord.shift = true;
    else if (vk == VK_LWIN || vk == VK_RWIN) chord.win = true;
    else if (vk == VK_RMENU) { chord.alt = true; chord.altVk = VK_RMENU; }
    else if (vk == VK_MENU || vk == VK_LMENU) { chord.alt = true; chord.altVk = VK_LMENU; }
    else chord.key = static_cast<WORD>(vk);
}

/// Sogou stores the voice shortcut as (modifiers << 8) + virtual key. Shift=1, Ctrl=2, Alt=4.
inline bool imeChordFromSogouCode(int code, ImeChord& chord) {
    chord = {};
    if (code <= 0) return false;
    const int mods = (code >> 8) & 0xFF;
    const int key = code & 0xFF;
    if (key <= 0) return false;
    if (mods & 1) chord.shift = true;
    if (mods & 2) chord.ctrl = true;
    if (mods & 4) chord.alt = true;
    if (mods & 8) chord.win = true;
    chord.key = static_cast<WORD>(key);
    return chord.used();
}

/// Read one small settings file as UTF-8, converting the UTF-16LE that some input methods write.
inline string imeTextFile(const wstring& file, size_t limit) {
    string bytes = SpeechService::read(file, limit);
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t count = (bytes.size() - 2) / sizeof(wchar_t);
        if (!count) return "";
        wstring wide(count, L'\0');
        memcpy(&wide[0], bytes.data() + 2, count * sizeof(wchar_t));
        return wideToUtf8(wide);
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) return bytes.substr(3);
    return bytes;
}

inline ImeChord sogouVoiceChord() {
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return {};
    const string text = imeTextFile(wstring(profile) + L"\\AppData\\LocalLow\\SogouPy\\extension.conf", 64 * 1024);
    const size_t name = text.find("com.sogou.voiceinput");
    if (name == string::npos) return {};
    const size_t next = text.find("\n[", name);
    const string section = text.substr(name, next == string::npos ? string::npos : next - name);
    if (section.find("shortcutKey_enable_1=bool:true") == string::npos) return {};
    const size_t key = section.find("shortcutKey1=int:");
    if (key == string::npos) return {};
    ImeChord chord;
    imeChordFromSogouCode(std::atoi(section.c_str() + key + 17), chord);
    return chord;
}

/// Doubao stores the live voice shortcut in its voice session snapshot.
inline ImeChord doubaoVoiceChord() {
    ImeChord chord; HKEY sessions = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DoubaoIme\\Interop\\Voice\\v1\\Sessions", 0, KEY_READ, &sessions) != ERROR_SUCCESS) return chord;
    wchar_t name[64]{}; wstring snapshot;
    for (DWORD index = 0; snapshot.empty() && RegEnumKeyW(sessions, index, name, 64) == ERROR_SUCCESS; ++index) {
        HKEY session = NULL;
        if (RegOpenKeyExW(sessions, name, 0, KEY_READ, &session) != ERROR_SUCCESS) continue;
        wchar_t data[8192]{}; DWORD bytes = sizeof(data), kind = 0;
        if (RegQueryValueExW(session, L"Snapshot", NULL, &kind, reinterpret_cast<BYTE*>(data), &bytes) == ERROR_SUCCESS && kind == REG_SZ) snapshot = data;
        RegCloseKey(session);
    }
    RegCloseKey(sessions);
    const string json = wideToUtf8(snapshot);
    const size_t single = json.find("\"singleClick\"");
    if (single == string::npos) return chord;
    const size_t end = json.find('}', single);
    const string section = json.substr(single, end == string::npos ? string::npos : end - single);
    const size_t keyPos = section.find("\"keyCode\"");
    int keyCode = 0;
    if (keyPos != string::npos) { const size_t colon = section.find(':', keyPos); if (colon != string::npos) keyCode = std::atoi(section.c_str() + colon + 1); }
    const size_t mods = section.find("\"modifiers\"");
    if (mods != string::npos) {
        size_t cursor = mods;
        while ((cursor = section.find("0x", cursor)) != string::npos) {
            imeApplyModifier(chord, static_cast<unsigned>(std::strtoul(section.c_str() + cursor, nullptr, 16)));
            cursor += 2;
        }
    }
    if (keyCode > 0) imeApplyModifier(chord, static_cast<unsigned>(keyCode));
    return chord;
}

inline ImeChord voiceChord(const string& model) {
    ImeChord chord;
    if (model == "wetype") { chord.ctrl = true; chord.shift = true; chord.win = true; return chord; }
    if (model == "windows") { imeChordFromLabel(L"Win + H", chord); return chord; }
    if (model == "doubao") return doubaoVoiceChord();
    if (model == "sogou") return sogouVoiceChord();
    if (model == "iflytek") {
        HKEY key = NULL; wchar_t text[128]{}; DWORD bytes = sizeof(text), kind = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\iFly Info Tek\\iFlyIME", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            if (RegQueryValueExW(key, L"iFlyImeVoiceShiftHotKey", NULL, &kind, reinterpret_cast<BYTE*>(text), &bytes) == ERROR_SUCCESS && kind == REG_SZ) imeChordFromLabel(text, chord);
            RegCloseKey(key);
        }
        return chord;
    }
    return chord;
}

inline void sendImeChord(const ImeChord& chord) {
    if (!chord.used()) return;
    WORD keys[6]{}; int count = 0;
    if (chord.ctrl) keys[count++] = VK_LCONTROL;
    if (chord.alt) keys[count++] = chord.altVk;
    if (chord.shift) keys[count++] = VK_LSHIFT;
    if (chord.win) keys[count++] = VK_LWIN;
    if (chord.key) keys[count++] = chord.key;
    INPUT input[6]{};
    for (int index = 0; index < count; ++index) {
        input[index].type = INPUT_KEYBOARD; input[index].ki.wVk = keys[index];
        if (keys[index] == VK_LWIN || keys[index] == VK_RMENU) input[index].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(static_cast<UINT>(count), input, sizeof(INPUT));
    Sleep(60);
    for (int index = 0; index < count; ++index) {
        const int reverse = count - 1 - index;
        input[index].type = INPUT_KEYBOARD; input[index].ki.wVk = keys[reverse];
        input[index].ki.dwFlags = KEYEVENTF_KEYUP;
        if (keys[reverse] == VK_LWIN || keys[reverse] == VK_RMENU) input[index].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(static_cast<UINT>(count), input, sizeof(INPUT));
}

/// Open the voice panel with that input method's own shortcut. A second press would close it, so an open panel is left alone.
inline HWND findVoiceWindow(const wchar_t* titleWanted, const wchar_t* klassWanted) {
    struct Query { const wchar_t* title; const wchar_t* klass; HWND found; };
    Query query{titleWanted, klassWanted, NULL};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* query = reinterpret_cast<Query*>(param);
        wchar_t title[256]{}, klass[128]{};
        GetWindowTextW(window, title, 256); GetClassNameW(window, klass, 128);
        if (query->title && wcscmp(title, query->title) != 0) return TRUE;
        if (query->klass && wcscmp(klass, query->klass) != 0) return TRUE;
        query->found = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.found;
}

inline bool showVoiceWindow(HWND window) {
    if (!window) return false;
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    return IsWindowVisible(window) != FALSE;
}

inline void holdRightAlt() {
    INPUT down{}; down.type = INPUT_KEYBOARD; down.ki.wScan = 0x38; down.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &down, sizeof(down));
    Sleep(220);
    down.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &down, sizeof(down));
}

inline void clickScreen(int x, int y) {
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN), originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return;
    INPUT down{}; down.type = INPUT_MOUSE;
    down.mi.dx = static_cast<LONG>((static_cast<double>(x - originX) * 65535.0) / (width - 1));
    down.mi.dy = static_cast<LONG>((static_cast<double>(y - originY) * 65535.0) / (height - 1));
    down.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &down, sizeof(down));
    INPUT up = down; up.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(up));
}

inline void clickSogouListen() {
    HWND window = NULL;
    for (int attempt = 0; attempt < 20 && !window; ++attempt) {
        window = findVoiceWindow(L"搜狗语音输入法-主面板", L"Chrome_WidgetWin_1");
        if (!window) Sleep(100);
    }
    if (!window) return;
    RECT rect{}; if (!GetWindowRect(window, &rect)) return;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width < 40 || height < 40) return;
    clickScreen(rect.left + width * 82 / 100, rect.top + height * 82 / 100);
}

inline void launchImeVoice(const string& model) {
    if (model == "iflytek") ShellExecuteW(NULL, L"open", L"C:\\Program Files (x86)\\iFlytek\\iFlyIME\\3.0.1750\\iFlyVoice.exe", NULL, NULL, SW_SHOWNORMAL);
    else if (model == "sogou") { ShellExecuteW(NULL, L"open", L"C:\\Program Files (x86)\\SogouInput\\Components\\ai_voice_input\\1.0.1.3272\\bin\\sogou_voice_assistant.exe", NULL, NULL, SW_SHOWNORMAL); clickSogouListen(); }
    else if (model == "wetype") sendImeChord(voiceChord(model));
    else if (model == "doubao") { showVoiceWindow(findVoiceWindow(NULL, L"OimeVoiceWaveWindow")); holdRightAlt(); }
    else sendImeChord(voiceChord(model));
}

inline void openImeVoice(const string& model, bool wait) {
    if (!imeResultModel(model)) return;
    const bool visible = imeVoiceVisible(model);
    static ULONGLONG lastLaunch = 0;
    const ULONGLONG now = GetTickCount64();
    if (!visible && now - lastLaunch > 3000) { lastLaunch = now; launchImeVoice(model); }
    if (!wait) return;
    auto deadline = GetTickCount64() + 4000;
    while (!imeVoiceVisible(model) && GetTickCount64() < deadline) Sleep(100);
    // A shortcut that arrives while the previous bar is still closing is dropped; try once more.
    if (!imeVoiceVisible(model)) {
        lastLaunch = GetTickCount64(); launchImeVoice(model);
        deadline = GetTickCount64() + 4000;
        while (!imeVoiceVisible(model) && GetTickCount64() < deadline) Sleep(100);
    }
    if (imeVoiceVisible(model)) Sleep(250);
}

struct ImeProfileHold { bool armed = false; DWORD type = 0; LANGID lang = 0; CLSID clsid{}; GUID profile{}; HKL layout = NULL; };
inline ImeProfileHold& fileProfileHold() { static ImeProfileHold value; return value; }

inline void imeRestoreProfile(ImeProfileHold& hold) {
    if (!hold.armed) return;
    bool uninit = false; imeThreadCom(uninit);
    ITfInputProcessorProfiles* profiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles)))) {
        ITfInputProcessorProfileMgr* manager = nullptr;
        if (SUCCEEDED(profiles->QueryInterface(IID_PPV_ARGS(&manager)))) {
            manager->ActivateProfile(hold.type, hold.lang, hold.clsid, hold.profile, hold.layout, TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
            manager->Release();
        }
        profiles->Release();
    }
    if (uninit) CoUninitialize();
    hold.armed = false;
}

/// Make the selected input method current so its voice shortcut is live, and remember the previous one.
inline void imeActivateModel(const string& model, ImeProfileHold& hold) {
    if (!imeResultModel(model) || model == "windows") return;
    bool uninit = false; imeThreadCom(uninit);
    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles)))) { if (uninit) CoUninitialize(); return; }
    ITfInputProcessorProfileMgr* manager = nullptr;
    if (FAILED(profiles->QueryInterface(IID_PPV_ARGS(&manager)))) { profiles->Release(); if (uninit) CoUninitialize(); return; }
    IEnumTfInputProcessorProfiles* enumerator = nullptr;
    if (SUCCEEDED(manager->EnumProfiles(0, &enumerator)) && enumerator) {
        TF_INPUTPROCESSORPROFILE profile{}; TF_INPUTPROCESSORPROFILE active{}; bool had = false; bool foundTarget = false; TF_INPUTPROCESSORPROFILE target{};
        while (enumerator->Next(1, &profile, NULL) == S_OK) {
            if (profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR) continue;
            if ((profile.dwFlags & TF_IPP_FLAG_ACTIVE) != 0) { active = profile; had = true; }
            const char* found = supportedImeModel(guidString(profile.clsid), guidString(profile.guidProfile));
            if (found && model == found) { target = profile; foundTarget = true; }
        }
        enumerator->Release();
        const bool same = foundTarget && had && IsEqualGUID(active.clsid, target.clsid) && IsEqualGUID(active.guidProfile, target.guidProfile);
        if (foundTarget && !same && SUCCEEDED(manager->ActivateProfile(target.dwProfileType, target.langid, target.clsid, target.guidProfile, target.hkl, TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE)) && had) {
            hold.armed = true; hold.type = active.dwProfileType; hold.lang = active.langid; hold.clsid = active.clsid; hold.profile = active.guidProfile; hold.layout = active.hkl;
        }
    }
    manager->Release(); profiles->Release();
    if (uninit) CoUninitialize();
}

inline void SpeechService::recordingStarted(const wstring& wav) {
    if (!safeRecording(wav, true)) return;
    string engine, tip, profile, device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.enabled || !state_.liveEnabled || stopping_ || state_.recording) return;
        engine = liveEngine_; tip = liveImeTip_; profile = liveImeProfile_; device = liveDevice_;
    }
    wstring resolved = findVirtualCableId(utf8ToWide(device));
    if (!resolved.empty()) device = wideToUtf8(resolved);
    const bool wantIme = engine == "ime";
    const bool network = wantIme && systemHasInternet();
    const bool deviceOk = wantIme && !resolved.empty();
    std::shared_ptr<ImeLiveOwner> starting;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.enabled || !state_.liveEnabled || stopping_ || state_.recording) return;
        state_.recording = true; state_.liveWav = wav; liveCancelled_ = false; imeFellBack_.store(false);
        armLive(wav, engine, tip, profile, device, network, deviceOk, starting);
        ++state_.revision; launchThread(); wake_.notify_all();
    }
    if (starting) {
        const char* selected = supportedImeModel(tip, profile);
        openImeVoice(selected ? selected : "wetype", false);
        starting->start();
    }
}

/// Play an existing 8 kHz recording through the same queue and device clock. Used by the local verification tool.
inline int imePlayExistingWav(const wstring& wav, const wstring& devicePrefix, string& textOut, const string& model = "wetype", const wstring& knownId = L"") {
    HANDLE file = CreateFileW(wav.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 2;
    LARGE_INTEGER length{}; GetFileSizeEx(file, &length);
    std::vector<char> bytes(static_cast<size_t>(length.QuadPart)); DWORD read = 0;
    const BOOL ok = length.QuadPart > 44 && length.QuadPart < 64 * 1024 * 1024 && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, NULL);
    CloseHandle(file);
    if (!ok || read != bytes.size()) return 2;
    size_t data = 0, samples = 0;
    for (size_t offset = 12; offset + 8 < bytes.size();) {
        const char* id = bytes.data() + offset; const uint32_t size = *reinterpret_cast<const uint32_t*>(bytes.data() + offset + 4);
        if (std::string(id, id + 4) == "data") { data = offset + 8; samples = size / 2; break; }
        offset += 8 + size + (size & 1);
    }
    if (!data || samples < 800) return 3;
    wstring deviceId = knownId.empty() ? findVirtualCableId() : knownId;
    if (deviceId.empty()) return 4;
    ImeFileSession::voiceOpened().store(0);
    // WeChat keeps the bar after a run but stops listening, so a fresh session needs a fresh bar.
    if (model == "wetype") resetImeVoice(model);
    openImeVoice(model, true);
    const bool opened = imeVoiceVisible(model);
    const bool micWarning = !opened && imeNoticeVisible(L"麦克风");
    ImeFileSession::voiceOpened().store(opened ? 1 : micWarning ? 3 : 2);
    // A voice panel that never appeared cannot transcribe anything, so nothing is played into the cable.
    if (!opened) {
        ImeFileSession::playedMs().store(0);
        ImeFileSession::totalMs().store(0);
        ImeFileSession::note(model, "", "");
        imeRestoreProfile(fileProfileHold());
        return micWarning ? 8 : 7;
    }
    auto owner = std::make_shared<ImeLiveOwner>(wav, deviceId, model, [](const char*) {}, true);
    ImeFileSession::playedMs().store(0);
    ImeFileSession::totalMs().store(static_cast<uint64_t>(samples) * 1000 / 8000);
    ImeFileSession::note(model, "", "");
    { std::lock_guard<std::mutex> lock(ImeFileSession::mutex()); ImeFileSession::owner() = owner; }
    ImeFileSession::running() = true;
    owner->start();
    const int16_t* pcm = reinterpret_cast<const int16_t*>(bytes.data() + data);
    const auto started = GetTickCount64();
    for (size_t offset = 0; offset < samples && !owner->stopped();) {
        const size_t block = std::min<size_t>(160, samples - offset);
        if (!owner->pushFit(pcm + offset, block)) { Sleep(5); continue; }
        offset += block;
    }
    if (!owner->stopped()) owner->finishTail(); else owner->requestStop();
    owner->join();
    { std::lock_guard<std::mutex> lock(ImeFileSession::mutex()); if (ImeFileSession::owner() == owner) ImeFileSession::owner().reset(); }
    ImeFileSession::running() = false;
    textOut = owner->text();
    // Playback is over, so the voice session ends here instead of leaving the panel recording.
    closeImeVoice(model);
    const unsigned elapsed = static_cast<unsigned>(GetTickCount64() - started);
    const unsigned expected = static_cast<unsigned>(samples / 8);
    int code = 0;
    if (elapsed + 500 < expected) code = 5;
    else if (ImeFileSession::voiceOpened().load() == 3) code = 8;
    else if (!opened) code = 7;
    else if (textOut.empty()) code = 6;
    imeRestoreProfile(fileProfileHold());
    return code;
}

/// Load the registered icon for one text service. The file comes from the system profile, not from this program.
inline HICON loadTipIcon(const string& tip, const string& profile) {
    if (tip.empty() || profile.empty()) return NULL;
    const wstring root = L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + utf8ToWide(tip) + L"\\LanguageProfile";
    HKEY languages = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root.c_str(), 0, KEY_READ, &languages) != ERROR_SUCCESS) return NULL;
    HICON icon = NULL;
    wchar_t language[64];
    for (DWORD index = 0; !icon && RegEnumKeyW(languages, index, language, 64) == ERROR_SUCCESS; ++index) {
        HKEY profileKey = NULL;
        const wstring key = root + L"\\" + language + L"\\" + utf8ToWide(profile);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &profileKey) != ERROR_SUCCESS) continue;
        wchar_t file[1024]{}; DWORD bytes = sizeof(file), kind = 0;
        if (RegQueryValueExW(profileKey, L"IconFile", NULL, &kind, reinterpret_cast<BYTE*>(file), &bytes) == ERROR_SUCCESS && kind == REG_SZ) {
            DWORD iconIndex = 0; bytes = sizeof(iconIndex);
            RegQueryValueExW(profileKey, L"IconIndex", NULL, NULL, reinterpret_cast<BYTE*>(&iconIndex), &bytes);
            ExtractIconExW(file, static_cast<int>(iconIndex), NULL, &icon, 1);
        }
        RegCloseKey(profileKey);
    }
    RegCloseKey(languages);
    return icon;
}

/// Small icon plus the system names of installed input methods this build can drive.
inline HICON supportedImeIcon(wstring& names, const string& preferred) {
    names.clear(); string tip, profile;
    for (const auto& ime : enumerateEnabledImes()) {
        if (!ime.supported) continue;
        if (!names.empty()) names += L"\n";
        names += ime.name;
        if (ime.model == preferred) { tip = ime.tip; profile = ime.profile; }
    }
    return tip.empty() ? NULL : loadTipIcon(tip, profile);
}
