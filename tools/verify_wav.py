"""Check recording format, signal level, and per-second RMS variation.

The known fake-phone tone used to pass the amplitude-only check. Constant RMS
across complete seconds now fails, even when a short final second differs.
Passing signal checks cannot establish a real phone source or two-way speech;
the caller must also confirm that the WAV is new and came from the real phone.

Usage:
    python tools/verify_wav.py                       # newest wav in recordings/
    python tools/verify_wav.py recordings/xxx.wav

Reading the output:
    verdict REAL AUDIO      -> expected format, signal level, varying full seconds
    verdict SUSPECT         -> insufficient evidence; never count as a real call
"""
from __future__ import annotations

import struct
import sys
import wave
from pathlib import Path

#: Project root used to resolve relative command-line recording paths.
BASE = Path(__file__).resolve().parent.parent
#: Default folder searched when no explicit WAV path is supplied.
REC = BASE / "recordings"


def newest() -> Path:
    """Return the newest root recording, or exit if no WAV is available."""
    files = sorted(REC.glob("*.wav"), key=lambda p: p.stat().st_mtime)
    if not files:
        raise SystemExit("no wav found in " + str(REC))
    return files[-1]


def main() -> int:
    """Print WAV signal checks and return zero only when every check passes."""
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    target = Path(sys.argv[1]) if len(sys.argv) > 1 else newest()
    if not target.is_absolute():
        target = BASE / target
    if not target.exists():
        print("MISSING:", target)
        return 2

    print("file      :", target)
    print("size      :", target.stat().st_size, "bytes")

    with wave.open(str(target), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)

    print("channels  :", channels)
    print("sampwidth :", width)
    print("rate      :", rate)
    print("frames    :", frames)
    if rate:
        print("duration  : %.1fs" % (frames / float(rate)))

    if channels != 1 or width != 2 or rate != 8000:
        print("verdict   : SUSPECT (expected mono / 8000Hz / 16-bit PCM)")
        return 1

    count = len(raw) // 2
    if count <= 0:
        print("EMPTY: no samples in the data chunk")
        return 1
    samples = struct.unpack("<%dh" % count, raw[: count * 2])

    peak = max(max(samples), -min(samples))
    peak_pct = 100.0 * peak / 32768.0
    energy = sum(value * value for value in samples)
    rms = (energy / float(len(samples))) ** 0.5
    print("peak      : %d (%.1f%% of full scale)" % (peak, peak_pct))
    print("rms       : %.1f" % rms)

    step = rate or 8000
    print("--- per-second rms (first 60s) ---")
    quiet = 0
    total = 0
    full_second_rms: list[float] = []
    for offset in range(0, len(samples), step):
        chunk = samples[offset:offset + step]
        if not chunk:
            break
        chunk_energy = sum(value * value for value in chunk)
        value = (chunk_energy / float(len(chunk))) ** 0.5
        if len(chunk) == step:
            full_second_rms.append(value)
        total += 1
        if value < 30:
            quiet += 1
        if offset < step * 60:
            print("  %3ds  rms=%7.1f  peak=%6d"
                  % (offset // step, value, max(max(chunk), -min(chunk))))

    if total:
        print("quiet seconds (rms<30): %d/%d" % (quiet, total))

    # Compare full seconds only: a partial tail must not make a constant tone
    # look like speech. Two decimal places preserve the handoff's RMS criterion.
    distinct = len({round(value, 2) for value in full_second_rms})
    print("full-second rms kinds: %d/%d" % (distinct, len(full_second_rms)))
    if peak_pct <= 5.0 or rms <= 30:
        verdict = "SUSPECT (near silence)"
    elif len(full_second_rms) < 2:
        verdict = "SUSPECT (need at least two complete seconds to assess variation)"
    elif distinct <= 1:
        verdict = "SUSPECT (constant per-second RMS; possible synthetic tone)"
    else:
        verdict = "REAL AUDIO"
    ok = verdict == "REAL AUDIO"
    print("verdict   :", verdict)
    if ok:
        print("NOTE: signal checks passed; confirm new file and real-phone provenance")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
