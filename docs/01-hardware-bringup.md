# Step 1 — Hardware bring-up tests (speaker, camera)

Before building features, we verify each piece of hardware in isolation with a tiny
throwaway app. This page documents those bring-up tests.

App location convention: each test/feature is a self-contained TuyaOpen app under
`firmware/<name>/`, built **out-of-tree** (the app lives in this repo; the SDK stays
at `D:\tuyaopen`). Build from inside the app folder:

```powershell
$env:PYTHONUTF8 = 1
Set-Location D:\tuyaopen ; . .\export.ps1
Set-Location D:\HackStorm-2.0\firmware\<name>
tos.py build
tos.py flash   -p COM3 -b 921600
tos.py monitor -p COM4            # log baud is 460800; monitor sets it for you
```

---

## 1a. Speaker test — `firmware/speaker_test/` ✅

**Goal:** prove the speaker + audio DAC path work by playing a 2-second tone on boot.

### How it works

The T5 audio is driven through the **TDL audio driver layer** (`tdl_audio_manage.h`):

| Call | Purpose |
|------|---------|
| `board_register_hardware()` | brings up board peripherals, incl. the audio codec |
| `tdl_audio_find(AUDIO_CODEC_NAME, &h)` | get a handle to the codec |
| `tdl_audio_open(h, mic_cb)` | start it (a mic callback is required even if unused) |
| `tdl_audio_get_info(h, &info)` | query format: **16 kHz, 16-bit, mono, 20 ms frames, 640 B/frame** |
| `tdl_audio_volume_set(h, 80)` | set volume 0–100 |
| `tdl_audio_play(h, pcm, len)` | push a PCM frame to the speaker (blocks ~one frame) |

We generate the tone ourselves as **16-bit signed mono PCM**: a 1 kHz square wave
(flip the sample's sign every half-period). To play 2 s we push
`2000 ms / 20 ms = 100` frames of 320 samples each.

Key idea: `tdl_audio_play()` consumes roughly one frame-time per call, so looping it
100× naturally takes ~2 seconds. We reuse one static 640-byte buffer because the call
copies the data before returning.

### Result (verified 2026-05-23)

```
audio info: rate=16000 ch=1 bits=16 frame_ms=20 frame_size=640
>>> playing 1000 Hz tone for 2000 ms (100 frames of 320 samples)   @ 00:00:00
>>> tone finished                                                  @ 00:00:02
```

Start/finish timestamps are exactly 2 s apart → the tone played for the full duration.

### Gotchas / notes

- `tdl_audio_open()` **requires a mic callback** even for playback-only; we pass a
  no-op. (The board opens mic + speaker together.)
- The standard T5 codec format is **16 kHz / 16-bit / mono**. If you feed PCM at a
  different rate it will sound wrong — resample first.
- Volume is a separate knob from sample amplitude; we set both to comfortable values.

---

## 1b. Camera test — (planned)

> See the chat / next commit. On-device object detection is not practical on this MCU
> and the SDK ships no detection example, so the camera + "identify items" test uses
> the camera capture API (`tdl_camera_manage.h`) plus an off-board vision model.
