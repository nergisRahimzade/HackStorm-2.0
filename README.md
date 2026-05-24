# SmartCane — AI IoT Hackathon

An AI-powered smart cane that gives blind and visually impaired users **passive, always-on environmental awareness** — no sweeping required. Sensors mounted on the handle run continuously, forming a full coverage cone from floor level to overhead.

---

## Vision

> "We are living in the 21st century, with all the advanced technology around us used at almost every field and AI has become a part of our daily lives. In today's world blind people still have to sense the whole world with a cane. We wanted to maximize the efficiency and usage of the cane."

---

## Hardware

| Component | Role |
|---|---|
| HC-SR04 ultrasonic sensor | Forward-facing obstacle detection, floor to chest height |
| Camera (upward tilt ~30°) | Covers mid-body, overhangs, signs, and traffic lights |
| IMU (accelerometer + gyroscope) | Detects tilt, sudden drop, and fall events |
| Arduino Elegoo | Reads sensors and relays data to Python over serial USB |
| Laptop / PC | Runs Python — all computer vision and AI logic |
| Earpiece (bone conduction recommended) | Audio alerts — keeps ears open to the environment |

---

## Features

### Obstacle Detection
- **Sensor:** HC-SR04 ultrasonic
- Forward-fixed sensor alerts the user as they walk toward any object
- Beep rate increases with proximity: slow → fast → continuous
- Always scanning — no deliberate cane movement needed

### Overhead Hazard Detection
- **Sensor:** Camera
- Camera angled upward (~30°) continuously detects overhangs, open cabinet doors, scaffolding, and low-hanging branches
- Covers the zone a traditional cane never reaches

### Stair & Drop-off Detection
- **Sensor:** Camera
- Computer vision reads floor texture changes and step-edge patterns ahead of the user
- Alerts before the user reaches the transition point
- Long vibration pulse alert

### Elevator Arrival Alert
- **Sensor:** Camera
- Model recognizes elevator doors opening directly in front of the user
- Announces arrival via earpiece automatically
- No button-pressing or guessing required

### Traffic Light Detection
- **Sensor:** Camera
- Continuously reads pedestrian crosswalk signals
- Speaks "green light" when it is safe to cross
- Removes dependence on audible pedestrian signals

### Scene Reading *(button-triggered)*
- **Sensor:** Camera + GPT-4o Vision API
- On button press, captures a frame and describes the scene
- Speaks object names, positions, and recommended actions through the earpiece
- Example output: *"Bus 52, right in front of you, door open"* / *"Available seat, 30 cm to your left"*

### Fall Detection
- **Sensor:** IMU (accelerometer)
- Detects a sudden cane drop or impact event
- Triggers an SOS notification to a companion phone app with GPS location

---

## Alert System

| Alert type | Meaning |
|---|---|
| Slow → fast → continuous beep | Obstacle closing in |
| Short vibration pulse | Obstacle nearby |
| Long vibration pulse | Stair or drop-off ahead |
| Voice (earpiece) | Elevator arrival, green light, scene reading output |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Computer vision | Python, OpenCV, YOLOv8 (Ultralytics) |
| Scene reading | OpenAI GPT-4o Vision API |
| Serial communication | pyserial |
| Text-to-speech | pyttsx3 |
| Microcontroller | Arduino (C++ / .ino sketch) |
| Environment config | python-dotenv |

---

## Project Structure

```
HackStorm-2.0/
├── features/
│   ├── overhead_hazard.py
│   ├── stair_detection.py
│   ├── elevator_alert.py
│   ├── traffic_light.py
│   ├── scene_reading.py
│   └── fall_detection.py
├── utils/
│   ├── alert.py          # shared alert() helper
│   ├── camera.py         # shared camera stream
│   └── serial_reader.py  # Arduino serial input
├── arduino/
│   └── sensors.ino       # ultrasonic + IMU sketch
├── main.py               # entry point
├── .env                  # GROQ_API_KEY
├── .gitignore
└── README.md
```

---

## Setup & Running the Localhost Server

After cloning the repo:

**1. Install Python dependencies**
```bash
pip install flask opencv-python python-dotenv
```

**2. Create a `.env` file in the project root**
```
GROQ_API_KEY=gsk_...
```

`GROQ_API_KEY` is used for vision analysis (Llama 4 Scout via Groq).

**3. Run the server**
```bash
# From the project root
python tools/smart_cane_app.py

# Optional flags
python tools/smart_cane_app.py --camera 0 --interval 3
```

The live UI will be available at **http://localhost:5000**

The T5AI firmware board connects to this server at `http://<your-laptop-IP>:5000/api/status`. Update `LAPTOP_IP` in `firmware/smart_cane_hw_output/src/hw_output.c` to match your machine's IP address.

---

## Common Errors

### `Flask is required. Run: pip install flask`
Flask is not installed.
```bash
pip install flask
```

### `OpenCV is required. Run: pip install opencv-python`
OpenCV is not installed.
```bash
pip install opencv-python
```

### `ERROR: prompt file not found: .../tools/scene_prompt_always_on.txt`
The scene prompt file is missing from the `tools/` directory. Ensure `tools/scene_prompt_always_on.txt` exists — it ships with the repo and should not be deleted.

### `[Camera] ERROR: cannot open camera 0.`
The webcam index is wrong or the camera is in use by another app. Try index `1`:
```bash
python tools/smart_cane_app.py --camera 1
```

### `ModuleNotFoundError: No module named 'dotenv'`
```bash
pip install python-dotenv
```

### `KeyError` or `401 Unauthorized` from Groq
API key is missing or invalid. Check that `.env` in the project root contains a valid `GROQ_API_KEY` value.

### `ModuleNotFoundError: No module named 'cv2'`
Same as the OpenCV error above — `cv2` is the import name for `opencv-python`:
```bash
pip install opencv-python
```

---

## Out of Scope

| Feature | Reason removed |
|---|---|
| Moving object tracking | Out of current scope |
| Always-on scene reading | Replaced with button-triggered version to reduce noise |
| Indoor QR navigation | Requires pre-installed infrastructure — not universally deployable |
| Surface / ramp detection | Cane tip already communicates surface changes via natural haptic feedback |
| Crowd density mode | Fully covered by obstacle and hazard detection |

---

*Built at HackStorm 2.0 — AIIoT track*
