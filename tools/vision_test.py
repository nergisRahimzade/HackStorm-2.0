#!/usr/bin/env python3
"""
vision_test.py - Validate the vision pipeline for SmartCane (Scene Reading).

HOST-side (laptop) test, decoupled from the T5 board. It grabs a frame from a
USB webcam and asks a vision model to identify the items shown. The same flow
is what the on-board "Scene Reading" feature will use later.

Dependencies: only OpenCV (cv2) + the Python standard library. No pip installs.

Free model providers (pick with --provider):
  groq    (DEFAULT) Free, global (incl. EU), no credit card. Llama 4 Scout vision.
          Get a key: https://console.groq.com/keys     env: GROQ_API_KEY
  gemini  Free & high quality, BUT free tier is NOT available in EU/UK/Switzerland.
          Get a key: https://aistudio.google.com/apikey env: GEMINI_API_KEY
  ollama  Fully offline, no key, no region limits. Needs Ollama installed + a
          vision model pulled (e.g. `ollama pull llava`). https://ollama.com
  openai  GPT-4o. Requires a *paid* OpenAI account.       env: OPENAI_API_KEY

Usage
-----
1. Probe cameras (no key needed), find the webcam index:
       python vision_test.py --probe

2. Run the test with the default free provider (Groq):
       # PowerShell:
       $env:GROQ_API_KEY = "gsk_..."
       python vision_test.py --camera 1

   Hold up ~2 items in front of the webcam first.

Other options: --provider, --model, --items N, --image PATH, --save PATH
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

WARMUP_FRAMES = 15  # webcams often return dark/empty first frames

# Per-provider defaults. "style" selects the request/response format.
PROVIDERS = {
    "groq": {
        "style": "openai",
        "url": "https://api.groq.com/openai/v1/chat/completions",
        "key_env": "GROQ_API_KEY",
        "model": "meta-llama/llama-4-scout-17b-16e-instruct",
        "key_url": "https://console.groq.com/keys",
    },
    "openai": {
        "style": "openai",
        "url": "https://api.openai.com/v1/chat/completions",
        "key_env": "OPENAI_API_KEY",
        "model": "gpt-4o",
        "key_url": "https://platform.openai.com/api-keys (paid account required)",
    },
    "gemini": {
        "style": "gemini",
        "url": "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent",
        "key_env": "GEMINI_API_KEY",
        "model": "gemini-2.5-flash",
        "key_url": "https://aistudio.google.com/apikey (not free in EU/UK/CH)",
    },
    "ollama": {
        "style": "ollama",
        "url": "http://localhost:11434/api/chat",
        "key_env": None,
        "model": "llava",
        "key_url": "install from https://ollama.com, then: ollama pull llava",
    },
}

PROMPT = (
    "You are the vision system of a smart cane that helps a blind user. "
    "The user is holding up about {n} item(s) to the camera. "
    "Identify each distinct item you can see. For each, give its name and a very "
    "short description (color / notable features). If unsure, say so. "
    "Reply as a short numbered list, then ONE plain sentence a blind user could "
    "hear, e.g. 'In front of you: a red mug and a TV remote.'"
)


# --------------------------------------------------------------------------- #
# Camera capture (OpenCV)
# --------------------------------------------------------------------------- #
def _open_capture(index):
    import cv2
    backend = cv2.CAP_DSHOW if sys.platform.startswith("win") else cv2.CAP_ANY
    return cv2.VideoCapture(index, backend)


def probe(max_index=4):
    import cv2
    found = []
    for idx in range(max_index):
        cap = _open_capture(idx)
        if not cap.isOpened():
            cap.release()
            continue
        frame = None
        for _ in range(WARMUP_FRAMES):
            ok, f = cap.read()
            if ok and f is not None:
                frame = f
            time.sleep(0.03)
        cap.release()
        if frame is not None:
            h, w = frame.shape[:2]
            out = f"probe_cam{idx}.jpg"
            cv2.imwrite(out, frame)
            print(f"  [camera {idx}] OK  {w}x{h}  -> saved {out}")
            found.append(idx)
        else:
            print(f"  [camera {idx}] opened but no frame")
    if not found:
        print("No working cameras found. Is another app using the webcam?")
    else:
        print(f"\nWorking camera index(es): {found}")
        print("Open the probe_camN.jpg files to see which is the USB webcam,")
        print("then run:  python vision_test.py --camera <that index>")
    return found


def capture_jpeg(index, save_path):
    import cv2
    cap = _open_capture(index)
    if not cap.isOpened():
        raise RuntimeError(f"Could not open camera index {index}. Try --probe.")
    frame = None
    for _ in range(WARMUP_FRAMES):
        ok, f = cap.read()
        if ok and f is not None:
            frame = f
        time.sleep(0.03)
    cap.release()
    if frame is None:
        raise RuntimeError("Camera opened but returned no frame.")
    ok, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
    if not ok:
        raise RuntimeError("Failed to JPEG-encode the frame.")
    data = buf.tobytes()
    with open(save_path, "wb") as fh:
        fh.write(data)
    h, w = frame.shape[:2]
    print(f"Captured {w}x{h} frame ({len(data)} bytes) -> {save_path}")
    return data


# --------------------------------------------------------------------------- #
# Vision model request (one helper per API style)
# --------------------------------------------------------------------------- #
def _post(url, payload, headers, timeout=90):
    # Some providers sit behind Cloudflare, which blocks urllib's default
    # "Python-urllib/x.y" User-Agent (HTTP 403, error code 1010). Pretend to be
    # a normal browser so the request gets through.
    headers = dict(headers)
    headers.setdefault(
        "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    )
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        raise RuntimeError(f"API error {e.code}: {body}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"Network error: {e.reason}") from None


def ask(provider, model, jpeg_bytes, api_key, n_items):
    cfg = PROVIDERS[provider]
    b64 = base64.b64encode(jpeg_bytes).decode("ascii")
    prompt = PROMPT.format(n=n_items)

    if cfg["style"] == "openai":  # groq + openai share this format
        payload = {
            "model": model,
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}"}},
            ]}],
            "max_tokens": 500,
        }
        headers = {"Authorization": f"Bearer {api_key}",
                   "Content-Type": "application/json"}
        data = _post(cfg["url"], payload, headers)
        return data["choices"][0]["message"]["content"]

    if cfg["style"] == "gemini":
        url = cfg["url"].format(model=model) + f"?key={api_key}"
        payload = {"contents": [{"parts": [
            {"text": prompt},
            {"inline_data": {"mime_type": "image/jpeg", "data": b64}},
        ]}]}
        data = _post(url, payload, {"Content-Type": "application/json"})
        return data["candidates"][0]["content"]["parts"][0]["text"]

    if cfg["style"] == "ollama":
        payload = {
            "model": model,
            "messages": [{"role": "user", "content": prompt, "images": [b64]}],
            "stream": False,
        }
        data = _post(cfg["url"], payload, {"Content-Type": "application/json"})
        return data["message"]["content"]

    raise RuntimeError(f"unknown style for provider {provider}")


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="Free vision-model webcam test")
    ap.add_argument("--probe", action="store_true", help="list cameras and exit")
    ap.add_argument("--camera", type=int, default=0, help="camera index (default 0)")
    ap.add_argument("--provider", default="groq", choices=list(PROVIDERS),
                    help="vision backend (default: groq, free & global)")
    ap.add_argument("--model", help="override the provider's default model")
    ap.add_argument("--items", type=int, default=2, help="how many items shown (hint)")
    ap.add_argument("--image", help="use this image file instead of the webcam")
    ap.add_argument("--save", default="capture.jpg", help="where to save the frame")
    args = ap.parse_args()

    if args.probe:
        probe()
        return 0

    cfg = PROVIDERS[args.provider]
    model = args.model or cfg["model"]

    # Resolve API key (ollama needs none).
    api_key = None
    if cfg["key_env"]:
        api_key = os.environ.get(cfg["key_env"])

    # Get the image bytes (from file or webcam) FIRST, so it's saved even on error.
    if args.image:
        with open(args.image, "rb") as fh:
            jpeg = fh.read()
        print(f"Using image file: {args.image} ({len(jpeg)} bytes)")
    else:
        jpeg = capture_jpeg(args.camera, args.save)

    if cfg["key_env"] and not api_key:
        print(
            f"\nERROR: provider '{args.provider}' needs an API key.\n"
            f"  Set it (PowerShell):  $env:{cfg['key_env']} = \"<your key>\"\n"
            f"  Get a free key:       {cfg['key_url']}\n"
            "The frame was still captured to disk so you can inspect it.",
            file=sys.stderr,
        )
        return 2

    print(f"Asking [{args.provider}] {model} to identify the items...\n")
    answer = ask(args.provider, model, jpeg, api_key, args.items)
    print("=" * 60)
    print(answer)
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
