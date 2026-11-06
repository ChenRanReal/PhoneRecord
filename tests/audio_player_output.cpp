// Verify real Windows waveform output with silent synthetic data and no product session.
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
using std::string;
using std::wstring;
/// Fixture filenames contain ASCII only.
static wstring utf8ToWide(const string& value) { return wstring(value.begin(), value.end()); }
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
/// Reject only the mapper in the second run, forwarding individual endpoints to real WinMM.
static bool rejectMapper = false;
static MMRESULT outputOpen(LPHWAVEOUT handle, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
    if (rejectMapper && id == WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    return waveOutOpen(handle, id, format, callback, instance, flags);
}
#define waveOutOpen outputOpen
#include "../cpp/audio_player.hpp"
/// Exercise default and recovered output, pause, seek, completion and repeat using actual hardware cursors.
int main() {
    const string path = "silent-output.wav";
    {
        std::ofstream file(path, std::ios::binary);
        const uint32_t riff = 32036, fmtSize = 16, bytes = 32000;
        const WAVEFORMATEX format = {WAVE_FORMAT_PCM, 1, 8000, 16000, 2, 16, 0};
        file.write("RIFF", 4); file.write(reinterpret_cast<const char*>(&riff), 4); file.write("WAVEfmt ", 8);
        file.write(reinterpret_cast<const char*>(&fmtSize), 4); file.write(reinterpret_cast<const char*>(&format), 16);
        file.write("data", 4); file.write(reinterpret_cast<const char*>(&bytes), 4);
        std::vector<char> silence(bytes); file.write(silence.data(), silence.size());
    }
    bool ok = true;
    for (bool recovered : {false, true}) {
        rejectMapper = recovered;
        WavPlayer player;
        ok = player.open(path, NULL) && player.play();
        if (ok) { Sleep(300); ok = player.pump() && player.position() > 0 && player.pause(); }
        const DWORD paused = player.position();
        if (ok) { Sleep(150); ok = player.position() == paused && player.seek(1500) && player.play(); }
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (ok && player.playing() && GetTickCount64() < deadline) { Sleep(25); ok = player.pump(); }
        ok = ok && !player.playing() && player.position() == 2000 && player.play() && player.stop() && player.position() == 0;
        if (!ok) { std::wcerr << L"FAIL: real output recovered=" << recovered << L" " << player.errorDetails() << L'\n'; break; }
        std::cout << "PASS: real Windows output recovered=" << recovered << " cursor/pause/seek/end/replay/stop\n";
    }
    DeleteFileW(utf8ToWide(path).c_str());
    return ok ? 0 : 1;
}
