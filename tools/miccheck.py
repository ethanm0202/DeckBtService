"""miccheck.py - Record from a Bluetooth headset microphone, the way a call does.

Records from the WASAPI capture endpoint whose name contains "Headset (" (or --device) while playing a
quiet 440 Hz tone into the same headset's playback endpoint, as a call app keeps both directions open.
That matters: with wideband (mSBC) voice the host must keep sending frames, and a headset that gets
none ends the link after a few seconds (for example, disconnection reason 0x13). If the tone is
audible, host-to-headset voice works too. --no-play records only.

The take is always saved as a WAV and judged:

  PASS   at least half the expected samples arrived and they carry signal (RMS above --threshold)
  FAIL   no or partial audio delivered, digital silence (every sample zero), or a level too low
  NODEV  no matching capture device exists (no Hands-Free link is up)

Speak into the headset while it records.

Exit codes: 0 PASS, 1 FAIL, 2 NODEV, 3 capture error.
"""

import argparse
import datetime
import os
import sys
import wave

import numpy as np
import sounddevice as sd

HERE = os.path.dirname(os.path.abspath(__file__))


def capture_devices():
    """(index, name, hostapi, default rate) for every capture device, WASAPI first."""
    apis = sd.query_hostapis()
    found = []
    for index, dev in enumerate(sd.query_devices()):
        if dev["max_input_channels"] > 0:
            found.append((index, dev["name"], apis[dev["hostapi"]]["name"], int(dev["default_samplerate"])))
    found.sort(key=lambda d: (0 if "WASAPI" in d[2] else 1, d[0]))
    return found


def pick_device(pattern):
    """First capture device matching pattern. The raw WDM-KS Hands-Free filter is never chosen:
    streaming it bypasses the audio engine, which is what asks Bluetooth for the voice link."""
    pattern = pattern.lower()
    for dev in capture_devices():
        if pattern in dev[1].lower() and "WDM-KS" not in dev[2]:
            return dev
    return None


def level_dbfs(samples):
    """RMS and peak of int16 samples in dBFS (-inf for all-zero input)."""
    if samples.size == 0:
        return float("-inf"), float("-inf")
    x = samples.astype(np.float64) / 32768.0
    rms = float(np.sqrt(np.mean(x * x)))
    peak = float(np.max(np.abs(x)))
    to_db = lambda v: 20.0 * np.log10(v) if v > 0 else float("-inf")
    return to_db(rms), to_db(peak)


def word_endings(samples, rate):
    """How spoken words end: fall times (ms) from speech level to the floor, the floor, and its
    flatness. Natural endings fade over 100-300 ms and room noise wanders; a noise gate or suppressor
    cuts in well under 100 ms to a flat floor (for example, shared mode processing typically achieves
    median 50 ms to a floor flat within 1 dB)."""
    hop = rate // 100
    if samples.size < hop * 50:
        return [], float("nan"), float("nan")
    x = samples.astype(np.float64) / 32768.0
    env = np.sqrt(np.mean(x[: x.size // hop * hop].reshape(-1, hop) ** 2, axis=1))
    db = 20.0 * np.log10(np.maximum(env, 1e-7))
    floor = float(np.percentile(db, 10))
    hi = float(np.percentile(db, 90)) - 12.0
    lo = floor + 6.0
    falls = []
    i = 0
    while i < db.size:
        if db[i] >= hi:
            j = i
            while j < db.size and db[j] >= lo:
                j += 1
            k = j - 1
            while k > i and db[k] < hi:
                k -= 1
            if j < db.size:
                falls.append((j - k - 1) * 10)
            i = j
        i += 1
    flatness = float(np.std(db[db < floor + 3.0]))
    return falls, floor, flatness


def pick_render(capture_name):
    """WASAPI playback endpoint of the same headset: the name inside the capture name's parentheses."""
    inner = capture_name[capture_name.find("(") + 1:capture_name.rfind(")")] if "(" in capture_name else capture_name
    apis = sd.query_hostapis()
    for index, dev in enumerate(sd.query_devices()):
        if dev["max_output_channels"] > 0 and "WASAPI" in apis[dev["hostapi"]]["name"] and inner and inner in dev["name"]:
            return index, dev["name"], int(dev["default_samplerate"]), min(2, dev["max_output_channels"])
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="Headset (", help="capture device name substring (default: 'Headset (', the Bluetooth headset mic)")
    parser.add_argument("--seconds", type=float, default=6.0, help="recording length")
    parser.add_argument("--threshold", type=float, default=-60.0, help="minimum RMS in dBFS for PASS")
    parser.add_argument("--list", action="store_true", help="list capture devices and exit")
    parser.add_argument("--out", default=os.path.join(HERE, "_build", "miccheck"), help="directory for the WAV")
    parser.add_argument("--no-play", action="store_true", help="record only; do not play the tone into the headset")
    parser.add_argument("--exclusive", action="store_true",
                        help="WASAPI exclusive mode: bypasses Windows' capture enhancements (noise suppression)")
    args = parser.parse_args()

    if args.list:
        for index, name, api, rate in capture_devices():
            print(f"  [{index:3}] {api:24} {rate:6} Hz  {name}")
        return 0

    dev = pick_device(args.device)
    if dev is None:
        print(f"MICCHECK NODEV: no capture device matching '{args.device}'. Capture devices:")
        for index, name, api, rate in capture_devices():
            print(f"  [{index:3}] {api:24} {rate:6} Hz  {name}")
        return 2
    index, name, api, rate = dev
    render = None if args.no_play else pick_render(name)
    mode = "exclusive" if args.exclusive else "shared"
    print(f"[*] recording {args.seconds:.1f} s from [{index}] {name} ({api} {mode}, {rate} Hz) - speak now")
    if render:
        print(f"[*] playing a quiet 440 Hz tone into [{render[0]}] {render[1]} ({render[2]} Hz) - you should hear it")
    elif not args.no_play:
        print("[!] no playback endpoint found for this headset: recording only")
    # Callback streams against a wall-clock deadline: an endpoint whose voice link carries no data
    # never completes a blocking read.
    chunks = []
    phase = [0]

    def tone(outdata, frames, t, status):
        n = np.arange(phase[0], phase[0] + frames)
        outdata[:] = (0.03 * np.sin(2.0 * np.pi * 440.0 * n / render[2])).astype(np.float32)[:, None]
        phase[0] += frames

    streams = []
    try:
        extra = sd.WasapiSettings(exclusive=True) if args.exclusive else None
        streams.append(sd.InputStream(samplerate=rate, channels=1, dtype="int16", device=index, extra_settings=extra,
                                      callback=lambda indata, frames, t, status: chunks.append(indata[:, 0].copy())))
        if render:
            streams.append(sd.OutputStream(samplerate=render[2], channels=render[3], dtype="float32",
                                           device=render[0], callback=tone))
        for s in streams:
            s.start()
        sd.sleep(int(args.seconds * 1000))
    except Exception as exc:  # PortAudio reports endpoint failures as generic errors
        print(f"MICCHECK ERROR: capture failed: {exc}")
        return 3
    finally:
        for s in streams:
            s.close()
    samples = np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int16)
    expected = int(args.seconds * rate)

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "mic-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".wav")
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(samples.tobytes())

    rms, peak = level_dbfs(samples)
    nonzero = int(np.count_nonzero(samples))
    print(f"[*] samples={samples.size}/{expected} ({100.0 * samples.size / max(expected, 1):.0f}%) nonzero={nonzero} "
          f"rms={rms:.1f} dBFS peak={peak:.1f} dBFS wav={path}")
    falls, floor, flatness = word_endings(samples, rate)
    if falls:
        print(f"[*] word endings: {len(falls)}, fall to floor median {int(np.median(falls))} ms (min {min(falls)}, "
              f"max {max(falls)}); floor {floor:.1f} dBFS, flatness {flatness:.1f} dB "
              f"(natural: 100-300 ms and a wandering floor; a gate: under 100 ms to a flat floor)")
    if samples.size == 0:
        print("MICCHECK FAIL: no audio delivered (the voice link carries no data to the host)")
        return 1
    if nonzero == 0:
        print("MICCHECK FAIL: digital silence (every sample zero): the voice path carries no data")
        return 1
    if rms < args.threshold:
        print(f"MICCHECK FAIL: level {rms:.1f} dBFS is below {args.threshold:.1f} dBFS")
        return 1
    if samples.size < expected // 2:
        print(f"MICCHECK FAIL: partial audio: signal present ({rms:.1f} dBFS) but only {samples.size} of {expected} "
              "samples arrived (the link dropped)")
        return 1
    print(f"MICCHECK PASS: {rms:.1f} dBFS RMS from {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
