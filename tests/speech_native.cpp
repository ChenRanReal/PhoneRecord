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
    ImePcmQueue queue;
    std::vector<int16_t> overflow(16100);
    for (int index = 0; index < 16100; ++index) overflow[index] = static_cast<int16_t>(index);
    const auto pushed = GetTickCount64();
    queue.pushDrop(overflow.data(), overflow.size());
    assert(GetTickCount64() - pushed < 1000);
    assert(queue.size() == kImeQueueSamples);
    int16_t oldest = 0; assert(queue.peek(&oldest, 1) == 1 && oldest == 100);
    std::vector<int16_t> tone(16000, 1000);
    ImePcmQueue realtime; realtime.pushDrop(tone.data(), tone.size()); realtime.pushDrop(tone.data(), tone.size());
    assert(realtime.size() == kImeQueueSamples);
    ImeResampler resampler; std::vector<float> frames(48000); size_t silence = 0;
    resampler.render(realtime, frames.size(), 48000, 1, frames.data(), &silence);
    assert(realtime.size() > 7000 && realtime.size() < 9000);
    assert(silence < 500);
    ImePcmQueue underrun; std::vector<int16_t> brief(800, 1000); underrun.pushDrop(brief.data(), brief.size());
    ImeResampler gap; std::vector<float> second(48000); size_t missing = 0;
    gap.render(underrun, second.size(), 48000, 1, second.data(), &missing);
    assert(underrun.size() == 0 && missing > 40000);
    assert(!routeLiveCaption({}).startIme);
    assert(routeLiveCaption({true, true, true, true}).startIme);
    assert(strcmp(routeLiveCaption({true, false, true, true}).phase, "ime_unsupported") == 0);
    assert(!routeLiveCaption({true, false, true, true}).startIme);
    assert(!routeLiveCaption({true, true, false, true}).startIme);
    assert(strcmp(routeLiveCaption({true, true, true, false}).phase, "ime_no_device") == 0);
    assert(!imeTextTimedOut(false, 60000) && !imeTextTimedOut(true, kImeTextBoundMs - 1) && imeTextTimedOut(true, kImeTextBoundMs));
    ImeChord chord{};
    assert(imeChordFromLabel(L"Ctrl + Shift + Alt + [", chord) && chord.ctrl && chord.shift && chord.alt && !chord.win && chord.key == VK_OEM_4);
    assert(imeChordFromLabel(L"Ctrl + Win + Shift", chord) && chord.ctrl && chord.win && chord.shift && chord.key == 0);
    assert(imeChordFromLabel(L"Win + H", chord) && chord.win && chord.key == 'H' && !chord.ctrl);
    assert(imeChordFromSogouCode(1345, chord) && chord.alt && chord.shift && !chord.ctrl && !chord.win && chord.key == 'A');
    assert(!imeChordFromSogouCode(0, chord));
    assert(supportedImeModel("{86598FB9-66A2-463E-B9C2-AEB906D477AD}", "{607FDF85-FCC8-4DBD-A365-41296F980C9C}") != nullptr);
    assert(std::string(supportedImeModel("{86598FB9-66A2-463E-B9C2-AEB906D477AD}", "{607FDF85-FCC8-4DBD-A365-41296F980C9C}")) == "wetype");
    assert(imeActiveModel("wetype") && !imeActiveModel("doubao") && !imeActiveModel("iflytek") && !imeActiveModel("sogou") && !imeActiveModel("windows") && !imeActiveModel(""));
    assert(imeResultModel("doubao") && imeResultModel("iflytek") && imeResultModel("sogou") && imeResultModel("windows") && !imeResultModel("microsoft"));
    assert(std::string(imeKnownModel("{9D2B2E2B-3C93-4D2F-9D35-6EEB85F0D2B0}", "{2B4D4B3A-4D4F-4C0A-8E66-7F771A2B9C10}")) == "doubao");
    assert(std::string(imeKnownModel("{B722B5D7-0C4C-4933-A7B9-DF8C91F2C643}", "{0C7479AF-F27F-488C-A46B-5BDA6BF43E50}")) == "iflytek");
    assert(std::string(imeKnownModel("{E7EA138E-69F8-11D7-A6EA-00065B844310}", "{E7EA138F-69F8-11D7-A6EA-00065B844311}")) == "sogou");
    assert(std::string(imeKnownModel("{DCBD6FA8-032F-11D3-B5B1-00C04FC324A1}", "{6A114E62-E11B-447F-9A58-2D354F5C9204}")) == "windows");
    assert(supportedImeModel("{9D2B2E2B-3C93-4D2F-9D35-6EEB85F0D2B0}", "{2B4D4B3A-4D4F-4C0A-8E66-7F771A2B9C10}") == nullptr);
    assert(supportedImeModel("{B722B5D7-0C4C-4933-A7B9-DF8C91F2C643}", "{0C7479AF-F27F-488C-A46B-5BDA6BF43E50}") == nullptr);
    assert(supportedImeModel("{E7EA138E-69F8-11D7-A6EA-00065B844310}", "{E7EA138F-69F8-11D7-A6EA-00065B844311}") == nullptr);
    assert(supportedImeModel("{DCBD6FA8-032F-11D3-B5B1-00C04FC324A1}", "{6A114E62-E11B-447F-9A58-2D354F5C9204}") == nullptr);
    assert(supportedImeModel("微信输入法", "{607FDF85-FCC8-4DBD-A365-41296F980C9C}") == nullptr);
    assert(imeKnownModel("微信输入法", "{607FDF85-FCC8-4DBD-A365-41296F980C9C}") == nullptr);
    const auto imeWav = root / L"recordings" / L"2026-11-06" / L"ime.wav";
    { std::ofstream out(imeWav, std::ios::binary); out << "RIFFime"; }
    const wstring senseJson = imeWav.wstring() + L".transcript.json";
    assert(SpeechService::write(senseJson, "{\"model\":\"sensevoice-int8-2024-07-17\",\"status\":\"complete\",\"text\":\"keep\"}"));
    const wstring qwenJson = imeWav.wstring() + L".qwen3asr.transcript.json";
    assert(SpeechService::write(qwenJson, "{\"model\":\"qwen3-asr\",\"text\":\"qwen\"}"));
    assert(!publishImeTranscript(imeWav.wstring(), {{1.0, "ime"}}, 1.0, true));
    assert(SpeechService::read(senseJson).find("sensevoice-int8-2024-07-17") != string::npos);
    assert(SpeechService::read(qwenJson).find("qwen") != string::npos);
    DeleteFileW(senseJson.c_str());
    assert(publishImeTranscript(imeWav.wstring(), {{1.25, "微信输入法"}}, 1.25, true));
    const string published = SpeechService::read(senseJson);
    assert(published.find("\"model\":\"wetype\"") != string::npos);
    assert(published.find("sensevoice") == string::npos);
    const wstring live = (root / L"recordings" / L"2026-11-06" / L"ime.live.txt").wstring();
    const string liveBytes = SpeechService::read(live);
    assert(liveBytes.size() >= 3 && static_cast<unsigned char>(liveBytes[0]) == 0xEF && liveBytes.find("\r\n") == string::npos || liveBytes.find("微信输入法") != string::npos);
    assert(static_cast<unsigned char>(liveBytes[0]) == 0xEF && static_cast<unsigned char>(liveBytes[1]) == 0xBB && static_cast<unsigned char>(liveBytes[2]) == 0xBF);
    assert(SpeechService::write(senseJson, "{\"model\":\"sensevoice-int8-2024-07-17\",\"status\":\"complete\",\"text\":\"keep\"}"));
    assert(publishImeTranscript(imeWav.wstring(), {{1.5, "微信输入法"}}, 1.5, false, "wetype", "微信输入法", "", true));
    assert(SpeechService::read(senseJson).find("\"model\":\"wetype\"") != string::npos);
    UpdateManifest manifest;
    const string fields = "\nfile=PhoneRecord.exe\nsize=3\nsha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    assert(!manifest.parse("PhoneRecord-update-v1\nversion=9.0.0\narch=x86" + fields));
    assert(manifest.parse("PhoneRecord-update-v1\nversion=9.0.0\narch=x64" + fields));
    puts("SPEECH_NATIVE_OK lazy_start=1 queue_resume=1 cancel_join=1 architecture_isolation=1");
}
