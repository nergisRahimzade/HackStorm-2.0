#!/usr/bin/env python3
"""
scene_server.py - SmartCane "Scene Reading" companion service (runs on the laptop).

Flow:
  board double-tap --HTTP POST /scene--> this service
     -> capture a multi-frame burst from the USB webcam
     -> Groq vision (Llama 4 Scout, multi-image) with scene_prompt.txt
     -> pull the spoken "SAY:" line
     -> Windows SAPI TTS  -> 16 kHz / 16-bit / mono PCM (matches the board codec)
     -> return raw PCM in the HTTP response; board plays it on the speaker

Dependencies: OpenCV (cv2) + Python standard library only. No pip installs.
TTS uses Windows' built-in System.Speech via PowerShell (offline, free).

Setup:
  $env:GROQ_API_KEY = "gsk_..."

Validate the whole AI path NOW, with just the webcam (no board):
  python scene_server.py --selftest          # captures, describes, SPEAKS it on the laptop

Run the service for the board:
  python scene_server.py                      # serves on http://0.0.0.0:8000
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from tts import synth_wav, wav_to_pcm   # shared TTS for notifications & alerts

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- config (override via CLI flags) ----
GROQ_URL = "https://api.groq.com/openai/v1/chat/completions"
DEFAULT_MODEL = "meta-llama/llama-4-scout-17b-16e-instruct"  # free, vision, <=5 imgs
DEFAULT_CAMERA = 1          # EMEET USB webcam (index 0 = built-in, came out black)
DEFAULT_FRAMES = 4          # burst size (Scout accepts up to 5 images)
FRAME_GAP_S = 0.4           # spacing between frames (~1.5 s total for 4 frames)
WARMUP_FRAMES = 12
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36")  # dodges Cloudflare 1010

PROMPT = open(os.path.join(HERE, "scene_prompt.txt"), encoding="utf-8").read()


# --------------------------------------------------------------------------- #
# 1. Capture a short burst of frames from the webcam
# --------------------------------------------------------------------------- #
def capture_burst(index, n_frames, gap_s):
    import cv2
    backend = cv2.CAP_DSHOW if sys.platform.startswith("win") else cv2.CAP_ANY
    cap = cv2.VideoCapture(index, backend)
    if not cap.isOpened():
        raise RuntimeError(f"Could not open camera index {index}.")
    for _ in range(WARMUP_FRAMES):          # let exposure settle
        cap.read()
        time.sleep(0.02)
    frames = []
    for i in range(n_frames):
        ok, frame = cap.read()
        if ok and frame is not None:
            ok2, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
            if ok2:
                frames.append(buf.tobytes())
        if i < n_frames - 1:
            time.sleep(gap_s)
    cap.release()
    if not frames:
        raise RuntimeError("Camera opened but returned no frames.")
    print(f"  captured {len(frames)} frame(s)")
    return frames


# --------------------------------------------------------------------------- #
# 2. Ask Groq (multi-image) for the structured description
# --------------------------------------------------------------------------- #
def groq_describe(frames, model):
    content = [{"type": "text", "text": PROMPT}]
    for jpeg in frames:
        b64 = base64.b64encode(jpeg).decode("ascii")
        content.append({"type": "image_url",
                        "image_url": {"url": f"data:image/jpeg;base64,{b64}"}})
    payload = {"model": model, "max_tokens": 400,
               "messages": [{"role": "user", "content": content}]}
    req = urllib.request.Request(
        GROQ_URL, json.dumps(payload).encode("utf-8"),
        {"Authorization": f"Bearer {os.environ['GROQ_API_KEY']}",
         "Content-Type": "application/json", "User-Agent": UA}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"Groq API {e.code}: {e.read().decode('utf-8','replace')}")
    return data["choices"][0]["message"]["content"]


def extract_say(text):
    for line in text.splitlines():
        if line.strip().upper().startswith("SAY:"):
            return line.split(":", 1)[1].strip()
    # Fallback: if the model ignored the format, just speak the whole thing.
    return text.strip() or "I can't read the scene clearly, please try again."


# Text -> speech (synth_wav / wav_to_pcm) lives in tts.py, imported above so the
# scene service and the alert system share one TTS implementation.


# --------------------------------------------------------------------------- #
# One full cycle
# --------------------------------------------------------------------------- #
def run_once(camera, frames, gap, model):
    burst = capture_burst(camera, frames, gap)
    desc = groq_describe(burst, model)
    print("----- scene description -----\n" + desc + "\n-----------------------------")
    say = extract_say(desc)
    print("SAY ->", say)
    wav = synth_wav(say)
    return desc, say, wav


# --------------------------------------------------------------------------- #
# HTTP server (the board hits this)
# --------------------------------------------------------------------------- #
def make_handler(args):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200); self.send_header("Content-Type", "text/plain")
            self.end_headers(); self.wfile.write(b"scene_server ok")

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
            try:
                if self.path.rstrip("/").endswith("say"):
                    # /say : dynamic notification/alert; body = UTF-8 text to speak.
                    text = body.decode("utf-8", "replace").strip() or "Notification."
                    pcm = wav_to_pcm(synth_wav(text))
                else:
                    # /scene : capture burst -> vision -> speak the description.
                    _, _, wav = run_once(args.camera, args.frames, FRAME_GAP_S, args.model)
                    pcm = wav_to_pcm(wav)
            except Exception as e:                  # ALWAYS return speakable audio
                print("ERROR:", e)
                try:
                    pcm = wav_to_pcm(synth_wav("Scene reading failed, please try again."))
                except Exception:
                    pcm = b""
            self.send_response(200)
            self.send_header("Content-Type", "audio/L16;rate=16000")
            self.send_header("Content-Length", str(len(pcm)))
            self.end_headers()
            self.wfile.write(pcm)

        def log_message(self, *a):
            pass
    return Handler


def main():
    ap = argparse.ArgumentParser(description="SmartCane Scene Reading service")
    ap.add_argument("--selftest", action="store_true",
                    help="run one cycle and SPEAK it locally (validate without the board)")
    ap.add_argument("--camera", type=int, default=DEFAULT_CAMERA)
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    if not os.environ.get("GROQ_API_KEY"):
        print('Set your key first:  $env:GROQ_API_KEY = "gsk_..."', file=sys.stderr)
        return 2

    if args.selftest:
        print("Self-test: hold up ~2 items; capturing a burst now...")
        _, say, wav = run_once(args.camera, args.frames, FRAME_GAP_S, args.model)
        try:
            import winsound
            print("Playing the spoken result on this laptop...")
            winsound.PlaySound(wav, winsound.SND_FILENAME)
        except Exception as e:
            print(f"(could not auto-play; WAV saved at {wav}) {e}")
        return 0

    print(f"Scene Reading service on http://0.0.0.0:{args.port}  (POST /scene)")
    print(f"Camera index {args.camera}, {args.frames}-frame burst, model {args.model}")
    ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(args)).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
