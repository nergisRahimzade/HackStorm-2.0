#!/usr/bin/env python3
"""
smart_cane_app.py — SmartCane: always-on Scene Reading with live web UI.

Continuously captures frames from the USB webcam, analyzes them with
Groq vision (Llama 4 Scout) every N seconds, and speaks the result
via Windows SAPI TTS when something important is detected.

Live results are shown at http://localhost:5000

Requirements:
    pip install flask opencv-python

Setup:
    $env:GROQ_API_KEY = "gsk_..."
    python smart_cane_app.py                          # default: camera 1, 5 s interval
    python smart_cane_app.py --camera 0 --interval 3  # faster, different camera
"""

import argparse
import base64
import collections
import json
import os
import queue
import re
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# Load .env from the project root (one level up from tools/)
from dotenv import load_dotenv
load_dotenv(os.path.join(HERE, "..", ".env"))

# ── Dependency checks ──────────────────────────────────────────────────────────

try:
    from flask import Flask, Response, stream_with_context
except ImportError:
    print("Flask is required.  Run:  pip install flask", file=sys.stderr)
    sys.exit(1)

try:
    import cv2
except ImportError:
    print("OpenCV is required.  Run:  pip install opencv-python", file=sys.stderr)
    sys.exit(1)

try:
    from tts import synth_wav
    TTS_OK = True
except ImportError:
    TTS_OK = False
    print("[WARN] tts.py not found — audio disabled.", file=sys.stderr)

# ── Constants ──────────────────────────────────────────────────────────────────

GROQ_URL       = "https://api.groq.com/openai/v1/chat/completions"
MODEL          = "meta-llama/llama-4-scout-17b-16e-instruct"
CAMERA_FPS     = 7          # webcam read rate
WARMUP_FRAMES  = 14         # frames to discard on startup for exposure to settle
BURST_SIZE     = 2          # frames per Groq request (2 = ~4 k tokens, fits free tier)
SCAN_INTERVAL  = 20.0       # seconds between analyses — free Groq tier: ~30k TPM
                             # each scan ≈ 4k tokens → max ~7 scans/min → 20s is safe
ALERT_COOLDOWN  = 20.0      # seconds before the same general SAY: line repeats
HAZARD_COOLDOWN = 10.0      # shorter cooldown for safety-critical hazard alerts
MAX_HISTORY    = 25

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)

_PROMPT_PATH = os.path.join(HERE, "scene_prompt_always_on.txt")
try:
    PROMPT = open(_PROMPT_PATH, encoding="utf-8").read()
except FileNotFoundError:
    print(f"ERROR: prompt file not found: {_PROMPT_PATH}", file=sys.stderr)
    sys.exit(1)

# ── Shared state ───────────────────────────────────────────────────────────────

# Frame buffer: written by camera thread, read by scanner + MJPEG threads.
_frame_lock  = threading.Lock()
_frame_deque = collections.deque(maxlen=int(CAMERA_FPS * 6))  # ~6 s of frames
_latest_jpeg = None                                            # most recent JPEG

# Scene state: written by scanner thread, read by Flask threads.
_state_lock    = threading.Lock()
_current_scene = {}
_status        = "starting"   # "starting"|"scanning"|"analyzing"|"alert"|"error"
_history       = collections.deque(maxlen=MAX_HISTORY)

# SSE clients: one queue per connected browser tab.
_clients_lock = threading.Lock()
_sse_clients  = []

# TTS: single consumer thread for sequential playback.
_tts_queue    = queue.Queue(maxsize=3)

# Cooldown: normalized SAY: text → last-spoken monotonic timestamp.
_said_lock = threading.Lock()
_last_said: dict[str, float] = {}

# Hazard / actionable keywords used for fallback importance scoring.
_IMPORTANT_WORDS = frozenset({
    "step", "drop", "dropoff", "stair", "stairs", "obstacle", "hazard",
    "bus", "car", "vehicle", "moving", "approaching", "open", "elevator",
    "traffic", "light", "exit", "door", "caution", "warning", "low",
    "overhead", "hanging", "seat", "available", "signal", "cross",
})

app = Flask(__name__)

# ── Camera thread ──────────────────────────────────────────────────────────────

def _camera_thread(camera_index: int) -> None:
    global _latest_jpeg, _status

    backend = cv2.CAP_DSHOW if sys.platform.startswith("win") else cv2.CAP_ANY
    cap = cv2.VideoCapture(camera_index, backend)

    if not cap.isOpened():
        with _state_lock:
            _status = "error"
        _broadcast({"type": "status", "status": "error",
                    "message": f"Cannot open camera {camera_index}."})
        print(f"[Camera] ERROR: cannot open camera {camera_index}.", file=sys.stderr)
        return

    print(f"[Camera] Opened index {camera_index}.  Warming up…")
    for _ in range(WARMUP_FRAMES):
        cap.read()
        time.sleep(0.03)

    with _state_lock:
        _status = "scanning"
    _broadcast({"type": "status", "status": "scanning"})
    print("[Camera] Ready.")

    interval = 1.0 / CAMERA_FPS
    while True:
        ok, frame = cap.read()
        if not ok or frame is None:
            time.sleep(0.1)
            continue
        ok2, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 60])
        if ok2:
            jpeg = buf.tobytes()
            with _frame_lock:
                _frame_deque.append((time.monotonic(), jpeg))
                _latest_jpeg = jpeg
        time.sleep(interval)


# ── Groq API call ──────────────────────────────────────────────────────────────

def _groq_call(jpegs: list[bytes]) -> str:
    content: list[dict] = [{"type": "text", "text": PROMPT}]
    for jpeg in jpegs:
        b64 = base64.b64encode(jpeg).decode("ascii")
        content.append({
            "type": "image_url",
            "image_url": {"url": f"data:image/jpeg;base64,{b64}"},
        })
    payload = {
        "model": MODEL,
        "max_tokens": 350,       # extra headroom for OVERHEAD/DROPOFF lines
        "messages": [{"role": "user", "content": content}],
    }
    req = urllib.request.Request(
        GROQ_URL,
        json.dumps(payload).encode("utf-8"),
        {
            "Authorization": f"Bearer {os.environ['GROQ_API_KEY']}",
            "Content-Type":  "application/json",
            "User-Agent":    UA,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        raise RuntimeError(
            f"Groq {e.code}: {e.read().decode('utf-8', 'replace')}"
        ) from None
    return data["choices"][0]["message"]["content"]


# ── Scene parser ───────────────────────────────────────────────────────────────

def _parse_hazard_line(line: str) -> dict:
    """Parse 'OVERHEAD: yes — some description' or 'DROPOFF: no' into a dict."""
    rest   = line.split(":", 1)[1].strip()
    active = rest.upper().startswith("YES")
    desc   = re.sub(r"^yes\s*[\u2014\-]+\s*", "", rest, flags=re.IGNORECASE).strip()
    return {"active": active, "description": desc if active and desc.upper() != "YES" else ""}


def _parse(raw: str) -> dict:
    """
    Parse model response into:
      { items, say, important, overhead: {active, description},
        dropoff: {active, description}, raw }
    """
    items: list[dict] = []
    say       = ""
    important = False
    overhead  = {"active": False, "description": ""}
    dropoff   = {"active": False, "description": ""}

    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        upper = line.upper()
        if upper.startswith("SAY:"):
            say = line.split(":", 1)[1].strip()
        elif upper.startswith("IMPORTANT:"):
            important = "YES" in upper
        elif upper.startswith("OVERHEAD:"):
            overhead = _parse_hazard_line(line)
        elif upper.startswith("DROPOFF:"):
            dropoff = _parse_hazard_line(line)
        elif re.match(r"^\d+[.)]\s+", line):
            body  = re.sub(r"^\d+[.)]\s+", "", line)
            parts = [p.strip() for p in body.split(" - ", 2)]
            items.append({
                "what":   parts[0] if len(parts) > 0 else body,
                "where":  parts[1] if len(parts) > 1 else "—",
                "action": parts[2] if len(parts) > 2 else "-",
            })

    # Model ignored format: speak the raw text rather than silence.
    if not say:
        say = raw.strip() or "Scene unclear, please try again."

    # Hazards always force IMPORTANT: yes.
    if overhead["active"] or dropoff["active"]:
        important = True

    # Fallback importance if model omitted the IMPORTANT: line.
    if not important:
        important = _score_importance(items, say)

    return {
        "items":     items,
        "say":       say,
        "important": important,
        "overhead":  overhead,
        "dropoff":   dropoff,
        "raw":       raw,
    }


def _score_importance(items: list[dict], say: str) -> bool:
    """Heuristic importance check when the model omits IMPORTANT:."""
    for item in items:
        action = item.get("action", "-")
        if action and action not in ("-", "—", ""):
            return True
        combined = (item.get("what", "") + " " + item.get("where", "")).lower()
        if any(w in combined for w in _IMPORTANT_WORDS):
            return True
    return any(w in say.lower() for w in _IMPORTANT_WORDS)


# ── Cooldown / dedup ───────────────────────────────────────────────────────────

def _normalize(say: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"[^\w\s]", "", say.lower())).strip()


def _should_speak(say: str, cooldown: float = ALERT_COOLDOWN) -> bool:
    """True if this text hasn't been spoken within `cooldown` seconds."""
    norm = _normalize(say)
    now  = time.monotonic()
    with _said_lock:
        if now - _last_said.get(norm, 0.0) < cooldown:
            return False
        _last_said[norm] = now
        # Prune entries older than 5 minutes.
        stale = [k for k, v in _last_said.items() if now - v > 300]
        for k in stale:
            del _last_said[k]
    return True


# ── SSE broadcast ──────────────────────────────────────────────────────────────

def _broadcast(event: dict) -> None:
    msg = f"data: {json.dumps(event)}\n\n"
    with _clients_lock:
        clients = list(_sse_clients)
    dead = []
    for q in clients:
        try:
            q.put_nowait(msg)
        except queue.Full:
            dead.append(q)
    if dead:
        with _clients_lock:
            for q in dead:
                try:
                    _sse_clients.remove(q)
                except ValueError:
                    pass


# ── TTS thread ─────────────────────────────────────────────────────────────────

def _tts_worker() -> None:
    while True:
        say = _tts_queue.get()
        if not TTS_OK:
            continue
        try:
            wav = synth_wav(say)
            if sys.platform.startswith("win"):
                import winsound
                winsound.PlaySound(wav, winsound.SND_FILENAME)
            else:
                import subprocess
                cmd = ["afplay", wav] if sys.platform == "darwin" else ["aplay", wav]
                subprocess.run(cmd, check=False, capture_output=True)
        except Exception as e:
            print(f"[TTS] {e}", file=sys.stderr)


def _speak(say: str) -> None:
    try:
        _tts_queue.put_nowait(say)
    except queue.Full:
        pass  # a more recent alert will follow; drop this one


# ── Scanner thread ─────────────────────────────────────────────────────────────

def _scanner_thread() -> None:
    global _current_scene, _status
    print(f"[Scanner] Model: {MODEL} | Interval: {SCAN_INTERVAL}s | Burst: {BURST_SIZE} frames")

    while True:
        time.sleep(SCAN_INTERVAL)

        with _state_lock:
            if _status == "error":
                continue

        # Sample BURST_SIZE evenly-spaced frames from the rolling buffer.
        with _frame_lock:
            frames = list(_frame_deque)
        if len(frames) < BURST_SIZE:
            continue
        step  = max(1, (len(frames) - 1) // (BURST_SIZE - 1))
        burst = [frames[min(i * step, len(frames) - 1)][1] for i in range(BURST_SIZE)]

        with _state_lock:
            _status = "analyzing"
        _broadcast({"type": "status", "status": "analyzing"})

        try:
            raw   = _groq_call(burst)
            scene = _parse(raw)
            ts    = time.strftime("%H:%M:%S")
            spoke = False

            # ── Hazard alerts (safety-critical, shorter cooldown) ──────────
            oh = scene["overhead"]
            do = scene["dropoff"]

            if do["active"]:
                desc = do["description"] or "drop-off"
                msg  = f"Caution, {desc} ahead."
                if _should_speak(f"DROPOFF:{desc}", HAZARD_COOLDOWN):
                    _speak(msg)
                    spoke = True
                    print(f"[Scanner] {ts}  [DROPOFF]  {msg}")

            if oh["active"]:
                desc = oh["description"] or "obstacle"
                msg  = f"Obstacle above you. {desc}."
                if _should_speak(f"OVERHEAD:{desc}", HAZARD_COOLDOWN):
                    _speak(msg)
                    spoke = True
                    print(f"[Scanner] {ts}  [OVERHEAD]  {msg}")

            # ── General scene alert ────────────────────────────────────────
            if scene["important"] and _should_speak(scene["say"]):
                _speak(scene["say"])
                spoke = True

            entry = {
                "time":      ts,
                "say":       scene["say"],
                "items":     scene["items"],
                "important": scene["important"],
                "overhead":  oh,
                "dropoff":   do,
                "spoke":     spoke,
            }

            with _state_lock:
                _current_scene = scene
                _history.appendleft(entry)
                _status = "alert" if spoke else "scanning"

            _broadcast({
                "type":      "scene",
                "scene":     scene,
                "time":      ts,
                "important": scene["important"],
                "spoke":     spoke,
            })

            if spoke:
                time.sleep(1.5)   # keep "Alert!" badge visible briefly

            with _state_lock:
                _status = "scanning"
            _broadcast({"type": "status", "status": "scanning"})

            tag = "ALERT" if spoke else "quiet"
            print(f"[Scanner] {ts}  [{tag}]  {scene['say'][:90]}")

        except Exception as e:
            print(f"[Scanner] ERROR: {e}", file=sys.stderr)
            with _state_lock:
                _status = "scanning"
            _broadcast({"type": "status", "status": "scanning"})


# ── HTML / CSS / JS (single-file, no template folder needed) ───────────────────

_HTML = """\
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SmartCane — Scene Reading</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    :root {
      --bg:       #0d0d12;
      --surface:  #13131b;
      --surface2: #1a1a24;
      --border:   #22222e;
      --text:     #e0e0ec;
      --muted:    #62627a;
      --green:    #3ecf6e;
      --blue:     #3c9eff;
      --red:      #ff5555;
      --yellow:   #ffd166;
    }

    body {
      background: var(--bg);
      color: var(--text);
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      height: 100dvh;
      display: flex;
      flex-direction: column;
      overflow: hidden;
    }

    /* ─ Header ─────────────────────────────────── */
    header {
      background: var(--surface);
      border-bottom: 1px solid var(--border);
      height: 52px;
      padding: 0 20px;
      display: flex;
      align-items: center;
      gap: 12px;
      flex-shrink: 0;
    }
    header h1 { font-size: 1.05rem; font-weight: 700; }
    header h1 span { color: var(--blue); }

    .dot {
      width: 9px; height: 9px;
      border-radius: 50%;
      flex-shrink: 0;
      transition: background 0.3s;
    }
    #status-text {
      font-size: 0.76rem;
      font-weight: 600;
      letter-spacing: 0.5px;
      text-transform: uppercase;
    }
    #last-update {
      margin-left: auto;
      font-size: 0.73rem;
      color: var(--muted);
    }

    .s-scanning  { background: var(--green); }
    .s-analyzing { background: var(--blue);  animation: blink 0.9s ease-in-out infinite; }
    .s-alert     { background: var(--red);   animation: blink 0.45s ease-in-out infinite; }
    .s-starting  { background: var(--muted); }
    .s-error     { background: var(--red); }
    @keyframes blink { 0%,100%{opacity:1} 50%{opacity:.3} }

    /* ─ Two-column layout ───────────────────────── */
    .layout {
      display: grid;
      grid-template-columns: 1fr 390px;
      flex: 1;
      overflow: hidden;
      min-height: 0;
    }

    /* ─ Camera panel ────────────────────────────── */
    .camera-wrap {
      background: #000;
      position: relative;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .camera-wrap img {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }
    /* Scanning sweep line overlay (purely visual) */
    .scan-line {
      position: absolute;
      left: 0; right: 0;
      height: 2px;
      background: linear-gradient(90deg, transparent, var(--blue) 50%, transparent);
      opacity: 0;
      animation: none;
    }
    .scanning .scan-line  { opacity: 0.4; animation: sweep 2.5s linear infinite; }
    .analyzing .scan-line { opacity: 0.7; animation: sweep 1.0s linear infinite; }
    @keyframes sweep { from{top:0} to{top:100%} }

    .cam-badge {
      position: absolute;
      bottom: 12px; left: 12px;
      background: rgba(0,0,0,0.6);
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 6px;
      padding: 5px 10px;
      font-size: 0.72rem;
      color: #ccc;
      backdrop-filter: blur(4px);
    }

    /* ─ Right panel ─────────────────────────────── */
    .right-panel {
      background: var(--surface);
      border-left: 1px solid var(--border);
      display: flex;
      flex-direction: column;
      overflow: hidden;
      min-height: 0;
    }

    /* ─ SAY box ─────────────────────────────────── */
    .say-box {
      padding: 16px 18px;
      border-bottom: 1px solid var(--border);
      flex-shrink: 0;
      transition: background 0.4s, border-color 0.4s;
      border-left: 4px solid var(--blue);
    }
    .say-box.alert-on {
      background: #170f0f;
      border-left-color: var(--red);
    }
    .say-label {
      font-size: 0.66rem;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: 0.9px;
      margin-bottom: 8px;
    }
    #say-text {
      font-size: 1.12rem;
      font-weight: 500;
      line-height: 1.55;
      color: var(--text);
      min-height: 2.2em;
      transition: color 0.3s;
    }
    #say-text.alert { color: var(--red); }

    /* ─ Scene items table ───────────────────────── */
    .table-wrap {
      padding: 14px 18px 0;
      flex: 1;
      overflow-y: auto;
      min-height: 0;
    }
    .section-label {
      font-size: 0.66rem;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: 0.9px;
      margin-bottom: 10px;
    }
    table { width: 100%; border-collapse: collapse; font-size: 0.83rem; }
    thead th {
      text-align: left;
      padding: 0 8px 8px;
      color: var(--muted);
      font-size: 0.73rem;
      font-weight: 500;
      border-bottom: 1px solid var(--border);
    }
    tbody td {
      padding: 7px 8px;
      border-bottom: 1px solid rgba(255,255,255,0.04);
      vertical-align: top;
      line-height: 1.4;
    }
    tbody tr:last-child td { border-bottom: none; }
    .c-what   { font-weight: 500; }
    .c-where  { color: #a0a0bc; }
    .c-action { color: var(--yellow); }
    .c-action.none { color: var(--muted); font-style: italic; }
    .empty-cell { color: var(--muted); font-style: italic; text-align: center; padding: 18px 0; }

    /* ─ History ─────────────────────────────────── */
    .history-wrap {
      border-top: 1px solid var(--border);
      padding: 12px 18px;
      flex-shrink: 0;
      max-height: 205px;
      overflow-y: auto;
    }
    .history-list { list-style: none; display: flex; flex-direction: column; gap: 5px; }
    .h-item {
      display: flex;
      gap: 9px;
      padding: 6px 9px;
      border-radius: 6px;
      background: var(--surface2);
      border-left: 3px solid var(--border);
    }
    .h-item.imp { border-left-color: var(--red); }
    .h-item.overhead { border-left-color: #ff9944; }
    .h-item.dropoff  { border-left-color: var(--red); }
    .h-time { font-size: 0.68rem; color: var(--muted); white-space: nowrap; padding-top: 2px; }
    .h-say  { font-size: 0.81rem; color: #c0c0d8; line-height: 1.4; }
    .h-tag  { font-size: 0.65rem; font-weight: 700; letter-spacing: 0.5px;
              padding: 1px 5px; border-radius: 3px; white-space: nowrap; margin-top: 2px; }
    .h-tag.overhead { background: #3a1f00; color: #ff9944; }
    .h-tag.dropoff  { background: #3a0000; color: #ff5555; }
    .h-tag.scene    { background: #0a1a2a; color: var(--blue); }

    /* ─ Hazard bar ────────────────────────────── */
    .hazard-bar {
      display: none;
      background: #180808;
      border-bottom: 1px solid #4a1010;
      padding: 7px 20px;
      gap: 10px;
      align-items: center;
      flex-wrap: wrap;
      flex-shrink: 0;
    }
    .hazard-chip {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 4px 11px;
      border-radius: 5px;
      font-size: 0.77rem;
      font-weight: 600;
      animation: blink 0.7s ease-in-out infinite;
    }
    .hazard-chip.overhead { background: #2e1500; color: #ff9944; border: 1px solid #5a3000; }
    .hazard-chip.dropoff  { background: #2e0000; color: #ff5555; border: 1px solid #5a0000; }

    /* ─ Scrollbars ──────────────────────────────── */
    ::-webkit-scrollbar { width: 3px; }
    ::-webkit-scrollbar-track { background: transparent; }
    ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 2px; }

    @media (max-width: 700px) {
      .layout { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <header>
    <div class="dot s-starting" id="dot"></div>
    <h1>Smart<span>Cane</span></h1>
    <span id="status-text">Starting…</span>
    <span id="last-update">—</span>
  </header>

  <!-- Hazard alerts bar (hidden when no active hazards) -->
  <div class="hazard-bar" id="hazard-bar"></div>

  <div class="layout">
    <!-- Camera feed -->
    <div class="camera-wrap scanning" id="cam-wrap">
      <img src="/video_feed" alt="Live camera feed">
      <div class="scan-line"></div>
      <div class="cam-badge" id="cam-badge">Initializing…</div>
    </div>

    <!-- Info panel -->
    <div class="right-panel">
      <!-- SAY: box -->
      <div class="say-box" id="say-box">
        <div class="say-label">Scene Description</div>
        <div id="say-text">Waiting for first scan…</div>
      </div>

      <!-- Scene items table -->
      <div class="table-wrap">
        <div class="section-label">Detected Items</div>
        <table>
          <thead>
            <tr>
              <th>What</th>
              <th>Where</th>
              <th>Action</th>
            </tr>
          </thead>
          <tbody id="items-body">
            <tr><td colspan="3" class="empty-cell">No items yet</td></tr>
          </tbody>
        </table>
      </div>

      <!-- Alert history -->
      <div class="history-wrap">
        <div class="section-label" style="margin-bottom:9px">Recent Detections</div>
        <ul class="history-list" id="history-list">
          <li class="h-item">
            <span class="h-say" style="color:var(--muted);font-style:italic">No detections yet</span>
          </li>
        </ul>
      </div>
    </div>
  </div>

  <script>
    const dot        = document.getElementById("dot");
    const statusText = document.getElementById("status-text");
    const lastUpdate = document.getElementById("last-update");
    const sayBox     = document.getElementById("say-box");
    const sayEl      = document.getElementById("say-text");
    const itemsBody  = document.getElementById("items-body");
    const historyEl  = document.getElementById("history-list");
    const camWrap    = document.getElementById("cam-wrap");
    const camBadge   = document.getElementById("cam-badge");

    const STATUS = {
      scanning:  { label: "Scanning",    cls: "s-scanning",  cam: "scanning"  },
      analyzing: { label: "Analyzing…",  cls: "s-analyzing", cam: "analyzing" },
      alert:     { label: "Alert!",      cls: "s-alert",     cam: "analyzing" },
      starting:  { label: "Starting…",   cls: "s-starting",  cam: ""          },
      error:     { label: "Camera Error",cls: "s-error",     cam: ""          },
    };

    function esc(s) {
      return String(s || "")
        .replace(/&/g, "&amp;").replace(/</g, "&lt;")
        .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    }

    function setStatus(s) {
      const st = STATUS[s] || { label: s, cls: "s-starting", cam: "" };
      dot.className        = "dot " + st.cls;
      statusText.textContent = st.label;
      camBadge.textContent   = st.label;
      // update scan-line animation class on camera wrapper
      camWrap.className = "camera-wrap " + (st.cam || "");
    }

    function renderHazards(overhead, dropoff) {
      const bar   = document.getElementById("hazard-bar");
      const chips = [];
      if (dropoff && dropoff.active) {
        const d = dropoff.description ? ` — ${dropoff.description}` : "";
        chips.push(`<span class="hazard-chip dropoff">⚠ DROP-OFF AHEAD${esc(d)}</span>`);
      }
      if (overhead && overhead.active) {
        const d = overhead.description ? ` — ${overhead.description}` : "";
        chips.push(`<span class="hazard-chip overhead">⚠ OVERHEAD HAZARD${esc(d)}</span>`);
      }
      if (chips.length) {
        bar.innerHTML = chips.join("");
        bar.style.display = "flex";
      } else {
        bar.style.display = "none";
      }
    }

    function renderScene(scene, isAlert) {
      sayEl.textContent = scene.say || "—";
      sayEl.className   = isAlert ? "alert" : "";
      sayBox.className  = "say-box" + (isAlert ? " alert-on" : "");

      const rows = scene.items || [];
      if (!rows.length) {
        itemsBody.innerHTML = '<tr><td colspan="3" class="empty-cell">Nothing notable detected</td></tr>';
        return;
      }
      itemsBody.innerHTML = rows.map(r => {
        const noAct = !r.action || r.action === "-" || r.action === "\u2014";
        return `<tr>
          <td class="c-what">${esc(r.what)}</td>
          <td class="c-where">${esc(r.where)}</td>
          <td class="c-action${noAct ? " none" : ""}">${esc(noAct ? "—" : r.action)}</td>
        </tr>`;
      }).join("");
    }

    function addHistory(time, say, important, overhead, dropoff) {
      // Remove "no detections yet" placeholder on first real entry.
      if (historyEl.children.length === 1 &&
          historyEl.firstElementChild.querySelector("[style]")) {
        historyEl.innerHTML = "";
      }
      const li = document.createElement("li");
      const isOverhead = overhead && overhead.active;
      const isDropoff  = dropoff  && dropoff.active;
      li.className = "h-item" + (isDropoff ? " dropoff" : isOverhead ? " overhead" : (important ? " imp" : ""));
      let tag = "";
      if (isDropoff)  tag = `<span class="h-tag dropoff">DROP-OFF</span>`;
      else if (isOverhead) tag = `<span class="h-tag overhead">OVERHEAD</span>`;
      else if (important)  tag = `<span class="h-tag scene">SCENE</span>`;
      li.innerHTML = `<span class="h-time">${esc(time)}</span>${tag}<span class="h-say">${esc(say)}</span>`;
      historyEl.prepend(li);
      while (historyEl.children.length > 20) {
        historyEl.removeChild(historyEl.lastChild);
      }
    }

    // ── SSE connection (auto-reconnect on drop) ──────────────────────────────
    function connect() {
      const src = new EventSource("/scene_feed");

      src.onmessage = function (e) {
        const d = JSON.parse(e.data);
        if (d.type === "status") {
          setStatus(d.status);
        } else if (d.type === "scene") {
          renderScene(d.scene, d.important);
          renderHazards(d.scene.overhead, d.scene.dropoff);
          addHistory(d.time, d.scene.say, d.important, d.scene.overhead, d.scene.dropoff);
          lastUpdate.textContent = "Updated " + d.time;
          setStatus(d.important ? "alert" : "scanning");
        }
        // "ping" → keep-alive, ignore
      };

      src.onerror = function () {
        src.close();
        setStatus("starting");
        setTimeout(connect, 3000);
      };
    }
    connect();
  </script>
</body>
</html>
"""

# ── Flask routes ───────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return _HTML, 200, {"Content-Type": "text/html; charset=utf-8"}


@app.route("/video_feed")
def video_feed():
    def gen():
        while True:
            with _frame_lock:
                frame = _latest_jpeg
            if frame:
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
                )
            time.sleep(0.1)

    return Response(gen(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/scene_feed")
def scene_feed():
    q = queue.Queue(maxsize=50)
    with _clients_lock:
        _sse_clients.append(q)

    def gen():
        # Immediately push the current state to the new client.
        with _state_lock:
            sc = dict(_current_scene)
            st = _status
        if sc:
            yield (
                f"data: {json.dumps({'type': 'scene', 'scene': sc, 'time': '—', 'important': sc.get('important', False)})}\n\n"
            )
        yield f"data: {json.dumps({'type': 'status', 'status': st})}\n\n"

        try:
            while True:
                try:
                    msg = q.get(timeout=25)
                    yield msg
                except queue.Empty:
                    yield 'data: {"type":"ping"}\n\n'   # keep connection alive
        finally:
            with _clients_lock:
                try:
                    _sse_clients.remove(q)
                except ValueError:
                    pass

    return Response(
        stream_with_context(gen()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/api/status")
def api_status():
    with _state_lock:
        payload = {
            "status":  _status,
            "current": _current_scene,
            "history": list(_history)[:5],
        }
    return json.dumps(payload), 200, {"Content-Type": "application/json"}


# ── Entry point ────────────────────────────────────────────────────────────────

def main() -> None:
    global SCAN_INTERVAL

    ap = argparse.ArgumentParser(description="SmartCane — always-on Scene Reading web app")
    ap.add_argument("--camera",   type=int,   default=1,   help="webcam index (default 1)")
    ap.add_argument("--interval", type=float, default=20.0, help="seconds between analyses (default 20; free Groq tier: keep >= 16)")
    ap.add_argument("--port",     type=int,   default=5000, help="web server port (default 5000)")
    args = ap.parse_args()

    SCAN_INTERVAL = args.interval

    if not os.environ.get("GROQ_API_KEY"):
        print(
            'ERROR: GROQ_API_KEY is not set.\n'
            '  PowerShell:  $env:GROQ_API_KEY = "gsk_..."',
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"SmartCane Scene Reading")
    print(f"  Camera:   index {args.camera}")
    print(f"  Interval: {args.interval}s")
    print(f"  Model:    {MODEL}")
    print(f"  TTS:      {'enabled' if TTS_OK else 'disabled (tts.py not found)'}")
    print()

    threading.Thread(target=_camera_thread, args=(args.camera,), daemon=True).start()
    threading.Thread(target=_tts_worker,    daemon=True).start()
    time.sleep(2.0)   # let camera warm up before first scan
    threading.Thread(target=_scanner_thread, daemon=True).start()

    print(f"Web UI →  http://localhost:{args.port}")
    print("Press Ctrl+C to stop.\n")
    app.run(host="0.0.0.0", port=args.port, threaded=True, use_reloader=False)


if __name__ == "__main__":
    main()
