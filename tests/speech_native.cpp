#define PHONERECORD_SPEECH_TESTING
#include "../cpp/phonerecord.cpp"
#include <cassert>

/// Native-only fixture may observe lazy thread admission and the owned process cancellation path.
struct SpeechFixture {
    /// Missing models still expose honest recording state and retain model-specific history jobs.
    static void hybrid(const wstring& config, const wstring& file) {
        SpeechService service; service.start(config);
        assert(service.configure(true,"auto","huggingface.co"));
        service.selectModel("qwen"); assert(service.model() == "qwen");
        HANDLE writing = CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(writing != INVALID_HANDLE_VALUE);
        service.recordingStarted(file); assert(service.state().recording && service.state().liveWav == file);
        assert(!service.state().busy && !service.livePending_);
        assert(!service.enqueue(file,false,"qwen"));
        CloseHandle(writing); service.recordingEnded(file);
        assert(!service.state().recording && service.state().waiting == 1);
        assert(service.queue_.front().model == "sense");
        assert(service.enqueue(file,false,"qwen")); assert(service.state().waiting == 2);
        service.selectModel("qwen17"); assert(service.model() == "qwen17");
        assert(service.enqueue(file,false,"qwen17")); assert(service.state().waiting == 3);
        assert(!service.enqueue(file,false,"qwen99"));
        service.stop();
        SpeechService restored; restored.start(config);
        assert(restored.model() == "qwen17" && restored.state().waiting == 3);
        assert(restored.queue_[0].model == "sense" && restored.queue_[1].model == "qwen" && restored.queue_[2].model == "qwen17");
        restored.cancel(); restored.stop();
        std::filesystem::create_directories(restored.root_ + L"\\qwen");
        assert(SpeechService::write(restored.root_ + L"\\qwen\\installed-v1","fixture"));
        assert(restored.readyFor("qwen") && !restored.readyFor("qwen17"));
        assert(SpeechService::write(restored.root_ + L"\\qwen\\installed-1.7b-v1","fixture"));
        assert(restored.readyFor("qwen") && restored.readyFor("qwen17"));
    }
    /// Turning off live captions preserves admitted history work and both modules' defaults.
    static void modulePreferences(const wstring& config,const wstring& file) {
        SpeechService service;service.start(config);
        assert(service.configureLive(true,"en","hf-mirror.com"));
        assert(service.configure(true,"ja","huggingface.co",true));
        assert(service.enqueue(file,false,"qwen17"));
        assert(service.enqueue(file,false,"sense"));service.cancelModel("sense");
        assert(service.pendingFor("qwen17") && !service.pendingFor("sense"));
        assert(service.configureLive(false,"zh","hf-mirror.com"));
        assert(service.state().waiting==1 && service.state().enabled && !service.state().liveEnabled);
        service.recordingStarted(file);service.recordingEnded(file);
        assert(!service.state().recording && service.state().waiting==1);
        service.stop();
        SpeechService restored;restored.start(config);
        assert(restored.state().waiting==1 && !restored.state().liveEnabled);
        assert(restored.language()=="ja" && restored.liveLanguage()=="zh");
        assert(restored.source()=="huggingface.co" && restored.liveSource()=="hf-mirror.com");
        restored.cancel();restored.stop();
    }
    /// Verify no thread or directory exists until explicit enablement; queue survives restart.
    static void admission(const wstring& config, const wstring& file) {
        SpeechService service; service.start(config);
        assert(!service.state().enabled && !service.thread_.joinable());
        assert(GetFileAttributesW(service.root_.c_str()) == INVALID_FILE_ATTRIBUTES);
        assert(!service.enqueue(file)); assert(!service.configure(true, "es", "huggingface.co"));
        assert(service.configure(true, "zh", "huggingface.co"));
        assert(service.enqueue(file)); assert(service.enqueue(file)); assert(service.state().waiting == 1);
        assert(!service.enqueue(file + L".txt"));
        service.stop();
        SpeechService restored; restored.start(config); assert(restored.state().waiting == 1);
        restored.cancel(); assert(restored.state().waiting == 0);
        assert(restored.configure(false, "auto", "huggingface.co")); restored.stop();
    }
    /// Kill a real hidden child on cancellation and ensure its handle settles promptly.
    static void cancellation(const wstring& config) {
        SpeechService service; service.start(config);
        std::filesystem::create_directories(service.root_);
        wchar_t system[32768]{}; GetSystemDirectoryW(system, 32768);
        bool result = true;
        std::thread active([&] { result = service.process(wstring(system) + L"\\cmd.exe", L"/d /c ping -n 60 127.0.0.1 >nul", 0, 10000); });
        Sleep(300); ++service.epoch_; active.join(); assert(!result);
        service.stop();
    }
    /// Preserve a terminal failure across preference edits and restart instead of showing unprepared/100%.
    static void failures(const wstring& config) {
        SpeechService service; service.start(config); assert(service.configure(true,"auto","huggingface.co"));
        service.observeProgress("{\"step\":\"runtime\",\"percent\":\"100\",\"phase\":\"runtime\"}");
        const string error = "{\"step\":\"http_failed\",\"percent\":\"100\",\"phase\":\"model\",\"resource\":\"model.int8.onnx\",\"origin\":\"huggingface.co\",\"operation\":\"download\",\"httpStatus\":\"503\",\"detail\":\"fixture unavailable\",\"nativeCode\":\"ProtocolError\"}";
        service.observeProgress(error);
        auto state = service.state(); assert(state.step == "http_failed" && state.failedStep == "model" && state.percent == 0);
        assert(state.httpStatus == "503" && state.resource == "model.int8.onnx" && state.detail == "fixture unavailable");
        assert(service.configure(true,"en","hf-mirror.com")); assert(service.state().step == "http_failed");
        std::filesystem::create_directories(service.root_); assert(SpeechService::write(service.root_ + L"\\install-error.json",error)); service.stop();
        SpeechService restored; restored.start(config); assert(restored.state().failureCode == "http_failed" && restored.state().failedStep == "model"); restored.stop();
        SpeechService missing; missing.start(config + L"-no-frame"); std::filesystem::create_directories(missing.root_);
        wchar_t system[32768]{}; GetSystemDirectoryW(system,32768);
        assert(!missing.process(wstring(system) + L"\\cmd.exe",L"/d /c exit 7",0,5000));
        assert(missing.state().failureCode == "process_failed" && missing.state().processExit == "7"); missing.stop();
    }
};

/// Exercise production queue/persistence/containment and cancellation with synthetic local paths only.
int main() {
    const auto root = std::filesystem::current_path() / L"speech-fixture";
    std::filesystem::create_directories(root / L"recordings" / L"2026-11-06");
    const auto file = root / L"recordings" / L"2026-11-06" / L"synthetic.wav";
    { std::ofstream out(file, std::ios::binary); out << "fixture"; }
    g_cfg.recordDir = wideToUtf8((root / L"recordings").wstring());
    assert(SpeechService::safeRecording(file.wstring()));
    HANDLE writing = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(writing != INVALID_HANDLE_VALUE); assert(!SpeechService::safeRecording(file.wstring())); CloseHandle(writing);
    assert(!SpeechService::safeRecording((root / L"outside.wav").wstring()));
    SpeechFixture::admission((root / L"config.ini").wstring(), file.wstring());
    SpeechFixture::cancellation((root / L"cancel.ini").wstring());
    SpeechFixture::failures((root / L"failure.ini").wstring());
    SpeechFixture::hybrid((root / L"hybrid.ini").wstring(),file.wstring());
    SpeechFixture::modulePreferences((root/L"modules.ini").wstring(),file.wstring());
    UpdateManifest manifest;
    const string fields = "\nfile=PhoneRecord.exe\nsize=3\nsha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    assert(!manifest.parse("PhoneRecord-update-v1\nversion=9.0.0\narch=x86" + fields));
    assert(manifest.parse("PhoneRecord-update-v1\nversion=9.0.0\narch=x64" + fields));
    puts("SPEECH_NATIVE_OK lazy_start=1 queue_resume=1 cancel_join=1 architecture_isolation=1");
}
