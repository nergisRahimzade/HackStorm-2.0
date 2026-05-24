#!/usr/bin/env python3
"""
tts.py - SmartCane text-to-speech for spoken notifications & alerts.

Uses the OpenAI TTS API (tts-1 model) to synthesize speech and resamples
the output to 16 kHz / 16-bit / mono PCM — exactly the format the T5
board's audio codec plays, so the board can play the bytes directly with
tdl_audio_play().

Notifications/alerts reach the user two ways:
  * Dynamic   - a feature sends arbitrary text -> TTS -> PCM -> board plays it.
  * Pre-baked - fixed, time-critical alerts are generated AHEAD of time into
                alerts/<name>.pcm so they can be flashed onto the board and
                played INSTANTLY, with no laptop/network round-trip. Safety
                alerts must never wait on Wi-Fi.

CLI:
  python tts.py "Green light"               # speak it on this laptop (test phrasing)
  python tts.py --bake                      # generate alerts/*.pcm (+ *.wav to listen)
  python tts.py --list                      # show the built-in alert phrases
  python tts.py --voices                    # list available OpenAI TTS voices
  python tts.py --voice shimmer "Fall detected"
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import urllib.request
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ALERTS_DIR = os.path.join(HERE, "alerts")

# OpenAI TTS configuration
OPENAI_TTS_URL    = "https://api.openai.com/v1/audio/speech"
OPENAI_TTS_MODEL  = "tts-1"      # fast neural TTS; swap to "tts-1-hd" for higher quality
OPENAI_TTS_VOICE  = "nova"       # default voice — clear, natural female
OPENAI_VOICES     = ["alloy", "ash", "coral", "echo", "fable", "nova", "onyx", "sage", "shimmer"]
OPENAI_SOURCE_RATE = 24000       # OpenAI PCM output sample rate
TARGET_RATE        = 16000       # T5 board codec requirement (16 kHz/16-bit/mono)

# Built-in SmartCane notification/alert phrases (name -> spoken text).
# Keep them SHORT and FACTUAL. These get pre-baked for instant playback.
ALERTS = {
    "ready":             "Smart cane ready.",
    "working":           "One moment.",
    "no_network":        "No network connection.",
    "scene_unavailable": "Scene reading unavailable, please try again.",
    "overhead_hazard":   "Caution, obstacle above you.",
    "dropoff":           "Caution, a step or drop just ahead.",
    "elevator_open":     "Elevator doors are open.",
    "green_light":       "Green light.",
    "red_light":         "Red light, please wait.",
    "fall_detected":     "Fall detected. Sending your location.",
    "battery_low":       "Battery low.",
}


def synth_wav(text, voice=None):
    """Synthesize `text` via OpenAI TTS; resample to 16 kHz/16-bit/mono WAV; return path."""
    td = tempfile.mkdtemp(prefix="tts_")
    wav_path = os.path.join(td, "o.wav")
    payload = {
        "model": OPENAI_TTS_MODEL,
        "input": text,
        "voice": voice or OPENAI_TTS_VOICE,
        "response_format": "pcm",   # raw 24 kHz / 16-bit / mono from OpenAI
    }
    req = urllib.request.Request(
        OPENAI_TTS_URL,
        json.dumps(payload).encode("utf-8"),
        {
            "Authorization": f"Bearer {os.environ['OPENAI_API_KEY']}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        pcm_24k = r.read()
    # Resample 24 kHz → 16 kHz via linear interpolation (numpy is available via opencv)
    samples = np.frombuffer(pcm_24k, dtype=np.int16).astype(np.float32)
    n_out = int(round(len(samples) * TARGET_RATE / OPENAI_SOURCE_RATE))
    t_in  = np.arange(len(samples))
    t_out = np.linspace(0, len(samples) - 1, n_out)
    samples_16k = np.interp(t_out, t_in, samples).astype(np.int16)
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)        # 16-bit
        w.setframerate(TARGET_RATE)
        w.writeframes(samples_16k.tobytes())
    return wav_path


def wav_to_pcm(wav_path):
    """Return raw PCM samples from a 16 kHz/16-bit/mono WAV (board-ready)."""
    with wave.open(wav_path, "rb") as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels()) != (16000, 2, 1):
            raise ValueError(
                f"expected 16k/16-bit/mono, got "
                f"{w.getframerate()}Hz/{w.getsampwidth()*8}-bit/{w.getnchannels()}ch")
        return w.readframes(w.getnframes())


def text_to_pcm(text, voice=None):
    """Convenience: text -> 16 kHz mono PCM bytes."""
    return wav_to_pcm(synth_wav(text, voice))


def list_voices():
    """Return the list of available OpenAI TTS voice names."""
    return OPENAI_VOICES


def bake(voice=None):
    """Pre-generate every ALERTS phrase into alerts/<name>.pcm (+ .wav)."""
    os.makedirs(ALERTS_DIR, exist_ok=True)
    for name, text in ALERTS.items():
        wav = synth_wav(text, voice)
        pcm = wav_to_pcm(wav)
        with open(os.path.join(ALERTS_DIR, name + ".pcm"), "wb") as f:
            f.write(pcm)
        shutil.copy(wav, os.path.join(ALERTS_DIR, name + ".wav"))
        print(f"  {name:18s} {len(pcm):6d} B  \"{text}\"")
    print(f"\nBaked {len(ALERTS)} alerts into {ALERTS_DIR}")


def main():
    ap = argparse.ArgumentParser(description="SmartCane TTS for notifications/alerts")
    ap.add_argument("text", nargs="?", help="speak this text on the laptop (test)")
    ap.add_argument("--bake", action="store_true", help="generate alerts/*.pcm")
    ap.add_argument("--list", action="store_true", help="show built-in alert phrases")
    ap.add_argument("--voices", action="store_true", help="list available OpenAI TTS voices")
    ap.add_argument("--voice", help=f"OpenAI voice name (see --voices); default: {OPENAI_TTS_VOICE}")
    a = ap.parse_args()

    if a.voices:
        print("\n".join(list_voices()) or "(none found)")
        return 0
    if a.list:
        for n, t in ALERTS.items():
            print(f"  {n:18s} {t}")
        return 0
    if a.bake:
        bake(a.voice)
        return 0
    if a.text:
        wav = synth_wav(a.text, a.voice)
        try:
            import winsound
            winsound.PlaySound(wav, winsound.SND_FILENAME)
        except Exception as e:
            print(f"(WAV saved at {wav}) playback note: {e}")
        return 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
