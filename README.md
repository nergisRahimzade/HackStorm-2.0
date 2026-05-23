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
├── .env                  # OPENAI_API_KEY
├── .gitignore
└── README.md
```

---

## Setup

**1. Install dependencies**
```bash
pip install opencv-python ultralytics pyserial pyttsx3 openai python-dotenv
```

**2. Add your OpenAI API key to `.env`**
```
OPENAI_API_KEY=sk-...
```

**3. Flash `arduino/sensors.ino` to the Elegoo** using the Arduino IDE

**4. Run the cane**
```bash
python main.py
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
