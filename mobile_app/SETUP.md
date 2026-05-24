# SmartCane Mobile App — Quick Setup

## Requirements
- Node.js 18+
- [Expo Go](https://expo.dev/go) installed on your **physical phone** (iOS or Android)
- A free [Twilio](https://www.twilio.com) account

---

## 1. Install dependencies

```powershell
cd mobile_app
npm install
```

## 2. Start the development server

```powershell
npx expo start
```

Scan the QR code with:
- **Android** → Expo Go app
- **iPhone** → Camera app (opens Expo Go automatically)

> **No emulator needed.** Expo Go on your real phone gives you genuine GPS and
> accelerometer readings. Android Studio / iOS Simulator can simulate GPS but
> accelerometer simulation is limited and not representative of real falls.

---

## 3. Configure Twilio

1. Sign up at <https://www.twilio.com/try-twilio> (free trial, ~$15 credit)
2. In the Twilio Console copy:
   - **Account SID** (starts with `AC…`)
   - **Auth Token**
   - **A Twilio phone number** (buy one free with trial credit)
3. Open the **Settings tab** in the app and paste those values.
4. Enter your emergency contact's phone number in **E.164 format** (e.g. `+1234567890`).

---

## 4. How fall detection works

| Parameter | Value | Source |
|-----------|-------|--------|
| Threshold | < 0.65 g magnitude | `smartCaneAccelerometer.ino` `FREE_FALL_G_THRESHOLD` |
| Sample rate | 40 Hz (every 25 ms) | `SAMPLE_INTERVAL_MS` |
| Cooldown | 10 seconds | prevents repeated alerts |

**Formula:** `magnitude = √(x² + y² + z²)`  
If `magnitude < 0.65`, a fall is detected and:
1. GPS coordinates are fetched
2. A Google Maps link is built
3. Twilio sends an SMS to your emergency contact

---

## 5. Test without falling

Tap **"Simulate Fall (Test SMS)"** on the Monitor tab. This fires the full
alert pipeline (GPS fetch → SMS send) without needing to drop the phone.

---

## 6. Connecting real GPS/accelerometer hardware (later)

When you connect external GPS and accelerometer hardware to your computer:
- The Arduino serial data can be forwarded to the phone via a small Python
  bridge (see `tools/smart_cane_app.py` for the pattern).
- Alternatively, replace the `Accelerometer.addListener` block in `App.js`
  with a WebSocket/HTTP poll to your Python server, and replace the
  `Location.getCurrentPositionAsync` call with the server-side GPS data.

---

## Project structure

```
mobile_app/
  App.js          ← main app: accelerometer, GPS, fall detection, SMS
  app.json        ← Expo configuration + permissions
  package.json    ← dependencies
  babel.config.js ← Babel preset
  SETUP.md        ← this file
```
