# Step 0 — TuyaOpen toolchain setup & first flash (Tuya T5-AI Board)

> Goal of this step: get a working build/flash/monitor pipeline for the **Tuya T5-AI
> Board** using the [TuyaOpen](https://github.com/tuya/tuyaopen) SDK, and prove it
> end-to-end by flashing a "hello world" demo and reading its boot log over serial.
>
> Platform used: **Windows 11**, native **PowerShell** (no WSL).
> Date completed: 2026-05-23.

---

## 0. The mental model (read this first)

TuyaOpen is **not** Arduino. It's a full C SDK + RTOS for the T5 chip (a Beken
BK7258, ARMv8-M @ 480 MHz). You don't write a single `.ino`; instead you build an
**app** (a folder with `src/` + a `CMakeLists.txt` + one or more `config/*.config`
files) against a **platform** (the chip SDK, downloaded on first build) and a
**board** (pin/peripheral definition, e.g. `TUYA_T5AI_BOARD`).

The single tool that drives everything is **`tos.py`**. The typical loop is:

```
tos.py config choice   # pick which board variant to build for
tos.py build           # compile -> produces a .bin
tos.py flash           # write the .bin to the board over USB
tos.py monitor         # watch the board's serial log
```

Everything below is the one-time setup needed before that loop works on Windows.

---

## 1. Prerequisites & host tools

| Tool   | Needed version | How we got it |
|--------|----------------|---------------|
| git    | ≥ 2.0          | already installed (2.40.0) |
| cmake  | ≥ 3.28         | already installed (4.0.2) |
| ninja  | ≥ 1.6          | already installed (1.11.1) |
| make   | ≥ 3.0          | **was missing** — installed via winget (see §4) |
| Python | 3.9–3.13       | 3.14.4 present (see warning in §3) |

**Why the D: drive?** TuyaOpen's docs forbid building from the `C:` drive on
Windows, and our `C:` was nearly full anyway. So the SDK lives at `D:\tuyaopen`.
Rule of thumb: **no spaces, no non-ASCII characters, not on C:** in the SDK path.

**Why native PowerShell (not Git Bash/WSL)?** TuyaOpen's build scripts assume
native Windows path handling and **break under Git Bash / MSYS2**. WSL would also
work but makes USB-serial flashing painful (it needs `usbipd` passthrough), and the
board's COM port is directly visible to Windows. So: plain PowerShell.

---

## 2. Clone the SDK

```powershell
git config --global http.postBuffer 524288000   # avoids clone stalls on big repos
git clone https://github.com/tuya/TuyaOpen.git D:\tuyaopen
```

Submodules (cJSON, littlefs, FlashDB, etc.) are pulled automatically later by
`tos.py check` / `tos.py build` — you don't need `--recursive`.

---

## 3. Activate the environment

From the SDK root, **dot-source** the activation script. This creates a Python
virtual env (`.venv`) on first run, installs requirements, and puts `tos.py` on PATH:

```powershell
Set-Location D:\tuyaopen
. .\export.ps1
tos.py version          # confirm it works
```

> **Note on Python 3.14:** TuyaOpen is tested on 3.9–3.13 and prints
> `Warning: Python 3.14 is outside the tested range ... (3.11 recommended)`.
> It worked for us anyway. If you later hit a Python-package build failure, install
> Python 3.11 and recreate `.venv`.

You must re-activate (`. .\export.ps1`) in **every new terminal**.

---

## 4. Install the one missing tool: GNU Make

`tos.py check` reported `[ERROR]: [make] not found`. We installed a modern make:

```powershell
winget install --id ezwinports.make -e --accept-source-agreements --accept-package-agreements
```

This installs **GNU Make 4.4.1** and adds it to PATH. **Restart your terminal** once
so the new PATH is picked up, then re-run `. .\export.ps1`.

Re-running `tos.py check` should now show all four tools OK:

```
[NOTE]: [git]   (2.40.0 >= 2.0.0) is ok.
[NOTE]: [cmake] (4.0.2 >= 3.28.0) is ok.
[NOTE]: [make]  (4.4.1 >= 3.0.0) is ok.
[NOTE]: [ninja] (1.11.1 >= 1.6.0) is ok.
```

---

## 5. Build a demo app

We used the SDK's minimal hello-world to prove the toolchain before writing our own
code: `examples/get-started/sample_project`. Its `app_default.config` already targets
the T5 (`CONFIG_BOARD_CHOICE_T5AI=y`), so no config step was needed.

```powershell
$env:PYTHONUTF8 = 1                      # <-- CRITICAL on Windows, see below
Set-Location D:\tuyaopen
. .\export.ps1
Set-Location D:\tuyaopen\examples\get-started\sample_project
tos.py build
```

The **first** build is slow: it also downloads the T5 platform SDK
(`github.com/tuya/TuyaOpen-T5AI`) and the ARM GCC toolchain (a few hundred MB).

### ⚠️ The `PYTHONUTF8=1` gotcha (Windows-specific)

Without it, the build compiles fine but **fails at the final packaging step** with:

```
错误: 打包失败: 'charmap' codec can't encode characters in position 5-6: ...
make: *** [tools/build_main.mk:168: package] Error 1
```

Cause: a packaging Python script prints Chinese log text, and Windows Python defaults
to the legacy `cp1252` encoding, which can't represent those characters.
`PYTHONUTF8=1` forces UTF-8 mode and fixes it.

A successful build ends with:

```
====================[ BUILD SUCCESS ]===================
 Target : sample_project_QIO_1.0.0.bin
```

---

## 6. Connect & identify the serial ports

The T5-AI Board uses a **CH342 dual USB-serial chip**, so one USB cable shows up as
**two** COM ports. List them in PowerShell:

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name
```

On this machine:

| Device                              | Port  | Role |
|-------------------------------------|-------|------|
| `USB-Enhanced-SERIAL-A CH342`       | COM3  | **download / flash** (the "A" port) |
| `USB-Enhanced-SERIAL-B CH342`       | COM4  | **log / monitor** (the "B" port) |

Rule (from TuyaOpen docs): the port whose name ends in **A is for flashing**, **B is
for logs**. Your COM numbers may differ — check Device Manager.

---

## 7. Flash the board

`tos.py flash` accepts `-p` (port) and `-b` (baud), so you can skip the interactive
port prompt:

```powershell
tos.py flash -p COM3 -b 921600
```

On first run it auto-downloads the flashing tool `tyutool`. A good flash looks like:

```
Waiting Reset ... unprotect flash OK ... sync baudrate 921600 success
Erase flash success ... Write flash success ... CRC check success ... Reboot done
Flash write success.
```

`CRC check success` is the important line — it means the firmware on the chip matches
the binary byte-for-byte.

---

## 8. Read the boot log (verification)

The log port (COM4) runs at **460800 baud** for the T5-AI — *not* 115200 or the
921600 flash baud. (Found in tyutool: `flash_interface.py` → `"T5AI": { "monitor_baudrate": 460800 }`.)
Using the wrong baud gives garbage characters.

Easiest way — let the tool pick the right baud, press the board's `RST` button to
reboot, and watch live:

```powershell
tos.py monitor -p COM4
# press RST on the board; quit with Ctrl+C
```

### Tip: reset the board from software (no button press)

The CH342 control lines are wired so **RTS = reset (CEN)** and **DTR = boot pin**. To
reboot into the app (not download mode), pulse RTS while keeping DTR low:

```powershell
$dl = New-Object System.IO.Ports.SerialPort('COM3',921600,'None',8,'One'); $dl.Open()
$dl.DtrEnable=$false; $dl.RtsEnable=$true; Start-Sleep -Milliseconds 100; $dl.RtsEnable=$false
$dl.Close()
```

### What a healthy boot log looks like

```
-------- app startup, left heap: 253056, reset reason: 0
go to tuya
[sample_project.c:38] Application information:
[sample_project.c:39] Project name:        sample_project
[sample_project.c:42] TuyaOpen version:    v1.6.0
[sample_project.c:44] Platform chip:       T5AI
[sample_project.c:45] Platform board:      TUYA_T5AI_BOARD
[sample_project.c:48] hello world
[sample_project.c:54] cnt is 1
[sample_project.c:62] cnt is 10
```

Seeing `hello world` + the platform info = the full pipeline works. ✅

---

## 9. Quick reference (every-session cheat sheet)

```powershell
# once per terminal:
$env:PYTHONUTF8 = 1
Set-Location D:\tuyaopen
. .\export.ps1

# then, from inside an app folder:
Set-Location D:\tuyaopen\examples\get-started\sample_project
tos.py build
tos.py flash   -p COM3 -b 921600
tos.py monitor -p COM4
```

| Thing            | Value |
|------------------|-------|
| SDK root         | `D:\tuyaopen` |
| Flash port       | `COM3` @ 921600 |
| Log port         | `COM4` @ 460800 |
| Board id         | `TUYA_T5AI_BOARD` |
| UTF-8 env var    | `PYTHONUTF8=1` (required before build) |

---

## 10. Notes for this project (SmartCane)

- The existing `smartCaneSensor/` folder is the **Obstacle Detection** feature written
  in **Arduino** (`.ino`). We are *not* changing it. Be aware it's a different
  framework from TuyaOpen — we'll decide later how the Arduino part and the TuyaOpen
  firmware coexist (separate MCU vs. porting to TuyaOpen).
- Board configs that already exist for our exact hardware:
  `TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config` (T5-AI board **with camera**), and
  `WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config` (for the round AMOLED board we'll add later).
- **Next step:** create our own TuyaOpen app skeleton (instead of building the SDK's
  sample), targeting the camera board config, as the foundation for the vision features.
