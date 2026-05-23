# Step 2 — Scene Reading service + TTS for notifications/alerts

Demo-oriented architecture (the heavy AI runs on the laptop; the board stays simple):

```
AMOLED board: double-tap → POST /scene → (plays returned PCM on speaker)
Laptop service (tools/scene_server.py):
    /scene → capture multi-frame burst from USB webcam → Groq vision (Llama 4
             Scout, multi-image) → take the "SAY:" line → TTS → 16 kHz PCM → board
    /say   → speak ANY text (notifications/alerts) → 16 kHz PCM → board
```

The board's camera isn't used — the **laptop USB webcam** is the scene camera.

## Text-to-speech (`tools/tts.py`) — the notification/alert engine

Every feature needs to speak (errors, "green light", "fall detected", …), so TTS is
its own reusable module. It uses **Windows SAPI** (`System.Speech`) via PowerShell:
offline, free, **no API key**. Output is **16 kHz / 16-bit / mono PCM** — the exact
format the board codec plays, so the board plays the bytes directly.

Two delivery modes:
- **Dynamic** — arbitrary text → speech on demand (`scene_server.py`'s `/say`).
- **Pre-baked** — fixed, time-critical alerts generated ahead of time into
  `alerts/<name>.pcm`, so the board can play them **instantly with no network**.
  Safety alerts must never wait on Wi-Fi. Regenerate with `python tts.py --bake`
  (the `alerts/` folder is git-ignored — it's a build artifact).

```powershell
python tts.py "Green light"          # hear a phrase on the laptop (tune wording)
python tts.py --list                 # the built-in alert phrases
python tts.py --voices               # Hazel (UK) / David (US m) / Zira (US f)
python tts.py --bake                 # generate alerts/*.pcm  (+ *.wav to listen)
python tts.py --voice "Microsoft Zira Desktop" "Fall detected"
```

Built-in alerts: `ready, working, no_network, scene_unavailable, overhead_hazard,
dropoff, elevator_open, green_light, red_light, fall_detected, battery_low`.

## Scene Reading service (`tools/scene_server.py`)

```powershell
$env:GROQ_API_KEY = "gsk_..."
python scene_server.py --selftest        # capture + describe + SPEAK locally (no board)
python scene_server.py                   # serve on http://0.0.0.0:8000 for the board
```

- **Multi-frame burst** (default 4 frames over ~1.5 s, webcam index 1) instead of one
  photo → more reliable, catches motion. Groq Llama 4 is image-only (max 5), so this is
  how we approximate "video".
- The prompt (`scene_prompt.txt`) is safety-tuned: *describe, never command*; state
  uncertainty; body-relative directions. The board only speaks the short `SAY:` line.
- Any failure path still returns speakable audio ("Scene reading failed…") — the device
  never goes silent.

## Status
- ✅ TTS verified: all 11 alerts baked as valid 16 kHz/16-bit/mono PCM.
- ⏳ Vision path needs a live run with your `GROQ_API_KEY` (`--selftest`) to confirm
  description quality before we wire the firmware.
