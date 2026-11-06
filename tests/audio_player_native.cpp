// Exercise the production streaming player with deterministic WinMM failures and synthetic WAVs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
using std::string;
using std::wstring;
/// Synthetic paths are ASCII; the product's Unicode conversion is covered by desktop workflows.
static wstring utf8ToWide(const string& value) { return wstring(value.begin(), value.end()); }
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)

/// WinMM state owned by this fixture, with no system device or user settings involved.
static struct AudioFixture {
    MMRESULT openError = 0, writeError = 0, prepareError = 0, resetError = 0, pauseError = 0, restartError = 0;
    MMRESULT mapperError = 0;
    UINT workingDevice = 0;
    unsigned opens = 0, handles = 0, prepared = 0, closes = 0;
    DWORD outputBytes = 0;
    UINT devices = 1, unit = TIME_BYTES;
    WAVEFORMATEX format{};
    std::vector<WAVEHDR*> queued;
    std::vector<int16_t> samples;
} fixture;
/// Simulate a stale default mapper and unavailable individual endpoints independently.
static MMRESULT fakeOpen(LPHWAVEOUT handle, UINT id, LPCWAVEFORMATEX format, DWORD_PTR, DWORD_PTR, DWORD flags) {
    if (flags != CALLBACK_NULL) throw std::runtime_error("unexpected mapper flags");
    ++fixture.opens;
    if (fixture.openError) return fixture.openError;
    if (id == WAVE_MAPPER && fixture.mapperError) return fixture.mapperError;
    if (id != WAVE_MAPPER && (id >= fixture.devices || id != fixture.workingDevice)) return MMSYSERR_BADDEVICEID;
    fixture.format = *format; ++fixture.handles; *handle = reinterpret_cast<HWAVEOUT>(1); return 0;
}
/// Simulate each buffer lifecycle operation without allowing premature buffer reuse.
static MMRESULT fakePrepare(HWAVEOUT, WAVEHDR* header, UINT) { if (fixture.prepareError) return fixture.prepareError; header->dwFlags |= WHDR_PREPARED; ++fixture.prepared; return 0; }
static MMRESULT fakeUnprepare(HWAVEOUT, WAVEHDR* header, UINT) { header->dwFlags &= ~WHDR_PREPARED; --fixture.prepared; return 0; }
static MMRESULT fakeReset(HWAVEOUT) { if (fixture.resetError) return fixture.resetError; for (auto* header : fixture.queued) header->dwFlags |= WHDR_DONE; fixture.queued.clear(); fixture.outputBytes = 0; return 0; }
static MMRESULT fakePause(HWAVEOUT) { return fixture.pauseError; }
static MMRESULT fakeRestart(HWAVEOUT) { return fixture.restartError; }
static MMRESULT fakeClose(HWAVEOUT) { --fixture.handles; ++fixture.closes; return 0; }
static UINT fakeDevices() { return fixture.devices; }
/// Capture actual output to verify channel mapping, interpolation, buffer boundaries and duration.
static MMRESULT fakeWrite(HWAVEOUT, WAVEHDR* header, UINT) {
    if (fixture.writeError) return fixture.writeError;
    if (!(header->dwFlags & WHDR_PREPARED) || std::find(fixture.queued.begin(), fixture.queued.end(), header) != fixture.queued.end()) throw std::runtime_error("invalid buffer ownership");
    header->dwFlags &= ~WHDR_DONE; fixture.queued.push_back(header);
    const auto* samples = reinterpret_cast<const int16_t*>(header->lpData);
    fixture.samples.insert(fixture.samples.end(), samples, samples + header->dwBufferLength / 2); return 0;
}
/// Return the chosen hardware cursor unit so source/output time mapping is tested independently.
static MMRESULT fakePosition(HWAVEOUT, MMTIME* time, UINT) {
    time->wType = fixture.unit;
    if (time->wType == TIME_BYTES) time->u.cb = fixture.outputBytes;
    else if (time->wType == TIME_SAMPLES) time->u.sample = fixture.outputBytes / fixture.format.nBlockAlign;
    else time->u.ms = fixture.outputBytes * 1000 / fixture.format.nAvgBytesPerSec;
    return 0;
}
#define waveOutOpen fakeOpen
#define waveOutPrepareHeader fakePrepare
#define waveOutUnprepareHeader fakeUnprepare
#define waveOutReset fakeReset
#define waveOutPause fakePause
#define waveOutRestart fakeRestart
#define waveOutClose fakeClose
#define waveOutGetNumDevs fakeDevices
#define waveOutWrite fakeWrite
#define waveOutGetPosition fakePosition
#include "../cpp/audio_player.hpp"

/// Fail with the synthetic assertion label only.
static void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
/// Write deterministic PCM; no fixture contains caller data or an audible real recording.
static void writeWav(const string& path, unsigned count, bool truncate = false) {
    std::ofstream output(path, std::ios::binary);
    const uint32_t bytes = count * 2, riff = bytes + 36, fmt = 16, rate = 8000, average = 16000;
    const uint16_t pcm = 1, channels = 1, alignment = 2, bits = 16;
    output.write("RIFF", 4); output.write(reinterpret_cast<const char*>(&riff), 4); output.write("WAVEfmt ", 8);
    output.write(reinterpret_cast<const char*>(&fmt), 4); output.write(reinterpret_cast<const char*>(&pcm), 2); output.write(reinterpret_cast<const char*>(&channels), 2);
    output.write(reinterpret_cast<const char*>(&rate), 4); output.write(reinterpret_cast<const char*>(&average), 4); output.write(reinterpret_cast<const char*>(&alignment), 2); output.write(reinterpret_cast<const char*>(&bits), 2);
    output.write("data", 4); output.write(reinterpret_cast<const char*>(&bytes), 4);
    for (unsigned i = 0; i < count - (truncate ? 1 : 0); ++i) { const int16_t sample = static_cast<int16_t>(static_cast<int>(i % 60000) - 30000); output.write(reinterpret_cast<const char*>(&sample), 2); }
}
/// Release resources before resetting the injected device state for another independent case.
static void reset(WavPlayer& player) { player.close(); require(!fixture.handles && !fixture.prepared, "leaked device/header"); fixture = {}; }
/// Verify byte-exact playback, invalid-default recovery, timing and independent device failures.
int main() {
    try {
        const string path = "synthetic-player.wav";
        writeWav(path, 12000);
        WavPlayer player;
        require(player.open(path, NULL) && player.play(), "native open/play");
        require(fixture.opens == 1 && fixture.format.nSamplesPerSec == 8000 && fixture.samples.size() == 8192, "native format/queue size");
        for (size_t i = 0; i < fixture.samples.size(); ++i) require(fixture.samples[i] == static_cast<int>(i) - 30000, "native samples changed");
        require(player.pause() && !player.playing() && player.play(), "pause/resume");
        reset(player);

        fixture.mapperError = MMSYSERR_BADDEVICEID; fixture.devices = 3; fixture.workingDevice = 1;
        require(player.open(path, NULL) && player.play(), "invalid mapper recovery");
        require(fixture.opens == 3 && fixture.format.nSamplesPerSec == 8000 && fixture.format.nChannels == 1, "fallback must stop at first valid device and preserve format");
        const auto initial = fixture.samples;
        for (UINT unit : {TIME_BYTES, TIME_SAMPLES, TIME_MS}) { fixture.unit = unit; fixture.outputBytes = 16000; require(player.position() == 1000 && player.length() == 1500, "cursor units"); }
        require(player.seek(250, true) && player.playing(), "fallback seek resume");
        require(player.position() == 250 && fixture.samples[initial.size()] == -28000, "seek cursor reset");
        while (player.playing()) { for (auto* header : fixture.queued) header->dwFlags |= WHDR_DONE; fixture.queued.clear(); require(player.pump(), "stream pump"); }
        require(player.position() == player.length() && player.play(), "end/replay");
        require(player.stop() && player.position() == 0 && !player.playing(), "stop/rewind");
        reset(player);

        for (const auto error : {MMSYSERR_NODRIVER, MMSYSERR_ALLOCATED, MMSYSERR_ERROR}) {
            fixture.openError = error; fixture.devices = error == MMSYSERR_ERROR ? 0 : 1;
            require(!player.open(path, NULL) && !player.loaded() && fixture.opens == 1, "unexpected retry or loaded device");
            require(string(player.errorKey()) == (error == MMSYSERR_ALLOCATED ? "message.player_busy" : "message.player_no_device"), "device error classification");
            require(player.errorDetails().find(L"operation=open") != wstring::npos, "open detail lost during cleanup"); reset(player);
        }
        fixture.mapperError = MMSYSERR_BADDEVICEID; fixture.devices = 0;
        require(!player.open(path, NULL) && fixture.opens == 1 && string(player.errorKey()) == "message.player_no_device", "missing endpoint"); reset(player);
        fixture.mapperError = MMSYSERR_BADDEVICEID; fixture.devices = 100; fixture.workingDevice = 99;
        require(!player.open(path, NULL) && fixture.opens == 33, "enumeration must be bounded"); reset(player);
        fixture.mapperError = MMSYSERR_BADDEVICEID; fixture.devices = 1;
        require(player.open(path, NULL), "replug recovery"); reset(player);
        fixture.openError = WAVERR_BADFORMAT;
        require(!player.open(path, NULL) && fixture.opens == 1 && string(player.errorKey()) == "message.player_format", "unsupported format must not trigger device scan"); reset(player);
        fixture.prepareError = MMSYSERR_NOMEM;
        require(!player.open(path, NULL), "prepare failure"); reset(player);
        require(player.open(path, NULL), "write failure setup"); fixture.writeError = MMSYSERR_ERROR;
        require(!player.play() && !player.playing(), "write failure state"); reset(player);
        require(player.open(path, NULL), "restart failure setup"); fixture.restartError = MMSYSERR_ERROR;
        require(!player.play() && !player.playing(), "restart failure state"); reset(player);
        require(player.open(path, NULL) && player.play(), "pause failure setup"); fixture.pauseError = MMSYSERR_ERROR;
        require(!player.pause(), "pause failure hidden"); reset(player);
        require(player.open(path, NULL), "reset failure setup"); fixture.resetError = MMSYSERR_ERROR;
        require(!player.seek(500), "reset failure hidden"); fixture.resetError = 0; reset(player);
        writeWav(path, 12000, true);
        require(!player.open(path, NULL) && !fixture.opens && string(player.errorKey()) == "message.player_invalid", "truncated file reached device"); reset(player);
        DeleteFileW(utf8ToWide(path).c_str());
        require(!player.open(path, NULL) && string(player.errorKey()) == "message.player_file", "missing file classification"); reset(player);
        writeWav(path, 1);
        require(player.open(path, NULL) && player.play() && fixture.samples.size() == 1, "single-sample playback"); reset(player);
        DeleteFileW(utf8ToWide(path).c_str());
        std::cout << "PASS: invalid mapper reproduction/recovery, bounded endpoint scan, unchanged samples, timing, seek/pause/replay, device errors and cleanup\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
