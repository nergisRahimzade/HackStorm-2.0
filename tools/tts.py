#!/usr/bin/env python3
"""
tts.py - SmartCane text-to-speech for spoken notifications & alerts.

Uses Windows' built-in System.Speech (SAPI) via PowerShell: offline, free, no
API key. Output is 16 kHz / 16-bit / mono PCM - exactly the format the T5
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
  python tts.py --voices                    # list installed SAPI voices
  python tts.py --voice "Microsoft Zira" "Fall detected"
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ALERTS_DIR = os.path.join(HERE, "alerts")

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
    """Synthesize `text` to a 16 kHz/16-bit/mono WAV file; return its path."""
    td = tempfile.mkdtemp(prefix="tts_")
    txt_path = os.path.join(td, "t.txt")
    wav_path = os.path.join(td, "o.wav")
    with open(txt_path, "w", encoding="utf-8") as fh:
        fh.write(text)
    select = f"$s.SelectVoice('{voice}');" if voice else ""
    ps = (
        "Add-Type -AssemblyName System.Speech;"
        "$fmt=New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo("
        "16000,[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,"
        "[System.Speech.AudioFormat.AudioChannel]::Mono);"
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        f"{select}"
        f"$s.SetOutputToWaveFile('{wav_path}',$fmt);"
        f"$s.Speak((Get-Content -Raw -Encoding UTF8 '{txt_path}'));"
        "$s.Dispose()"
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                   check=True, capture_output=True)
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
    ps = ("Add-Type -AssemblyName System.Speech;"
          "(New-Object System.Speech.Synthesis.SpeechSynthesizer)"
          ".GetInstalledVoices()|%{$_.VoiceInfo.Name}")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True)
    return [ln.strip() for ln in out.stdout.splitlines() if ln.strip()]


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
    ap.add_argument("--voices", action="store_true", help="list installed voices")
    ap.add_argument("--voice", help="voice name (see --voices)")
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
