# PianoAutomata — Solenoid Oracle

An art installation. A visitor types a question on a screen; an ESP32 sends it to the Gemini API, which answers as a rhythm of pulses. The ESP32 plays those pulses on a row of solenoids (piano "keys").

```
Browser (index.html) ──HTTP──▶ oracle.py ──USB serial──▶ ESP32 ──▶ Gemini API
                                                           │
                                                           └──▶ solenoids (GPIO 13, 14, 16, 17, 18)
```

## Repository layout

| Path | Purpose |
|------|---------|
| `arduino/arduino.ino` | ESP32 firmware: WiFi, Gemini request, solenoid control. |
| `frontend/oracle.py` | Python bridge: serves the page, forwards questions to the ESP32 over serial, tracks its status (idle / thinking / playing). |
| `frontend/index.html` | Fullscreen display: typewriter-style input, cycles through past questions while idle. |
| `frontend/questions.json` | Created and updated automatically; every question asked is saved here and reused on the idle screen. |

## Requirements

**Hardware**
- ESP32 board with a USB cable
- Up to 5 solenoids, each driven through a transistor/MOSFET with a flyback diode and its own power supply. Never connect a solenoid straight to a GPIO pin.
- Default GPIO pins: `13, 14, 16, 17, 18`. Edit `SOLENOID_PINS` in the `.ino` to change them. The rest of the code adapts automatically.

**Software**
- [Arduino IDE](https://www.arduino.cc/en/software) with the **esp32** board package (Boards Manager → "esp32 by Espressif Systems")
- Arduino library **ArduinoJson** (Library Manager)
- Python 3.9+ and `pyserial`
- A WiFi network with internet access
- A [Gemini API key](https://aistudio.google.com/apikey)

## 1. Flash the ESP32

1. Open `arduino/arduino.ino` in the Arduino IDE.
2. Near the top, set your own credentials:
   ```cpp
   const char* ssid     = "YOUR_WIFI_NAME";
   const char* password = "YOUR_WIFI_PASSWORD";
   const char* apiKey   = "YOUR_GEMINI_API_KEY";
   ```
3. Select your board (Tools → Board → ESP32 → e.g. "ESP32 Dev Module") and the serial port.
4. Upload.
5. Open the Serial Monitor at **115200 baud**. You should see `Connected. IP: ...` followed by `Waiting for input...`. Typing a question there and pressing Enter should make the solenoids fire after a few seconds.
6. **Close the Serial Monitor.** Only one program can hold the serial port at a time.

The model is set in `askGemini()` (`gemini-3.5-flash-lite`). If you get a 404 in the serial log, change the model ID to one your key can access.

## 2. Run the frontend

From the `frontend/` folder:

```bash
# first time only
python3 -m venv .venv
source .venv/bin/activate
pip install pyserial

# every time
source .venv/bin/activate
python oracle.py /dev/cu.wchusbserial10
```

Find the port name:

| OS | Typical port | How to list |
|----|--------------|-------------|
| macOS | `/dev/cu.usbserial-0001` or `/dev/cu.usbmodem...` | `ls /dev/cu.*` |
| Linux | `/dev/ttyUSB0` | `ls /dev/ttyUSB* /dev/ttyACM*` |
| Windows | `COM3` | Device Manager → Ports |

If you omit the port, it defaults to `/dev/ttyUSB0`.

Then open **http://localhost:8000** in a browser. Double-click the page for fullscreen.

## Using it

- **Ask:** just start typing (no input box needed), then press **Enter**.
- **Cancel typing:** press **Esc**. Half-typed input is also dropped after 30 seconds of inactivity.
- While the ESP32 is answering, input is ignored; the question stays on screen and the dot in the top-right pulses.
- When idle, the screen types out previously asked questions in random order.

Status dot (top right):

| Dot | Meaning |
|-----|---------|
| Dim blue | Idle, ready for a question |
| Pulsing blue | ESP32 is thinking or playing |
| Red | Browser can't reach `oracle.py` |

## Tuning

- **Animation timing** (typing speed, hold time, etc.): constants at the top of the `<script>` in `index.html`.
- **Pulse behaviour** (sequence length, durations, gaps, mood): the prompt in `askGemini()` in the `.ino`.
- **HTTP port:** `HTTP_PORT` in `oracle.py` (default 8000). The server only listens on `localhost`.
- **Reset the idle rotation:** delete or edit `frontend/questions.json`.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `serial.SerialException: could not open port` / "resource busy" | Close the Arduino Serial Monitor and any other program using the port; check the port name. |
| `ModuleNotFoundError: No module named 'serial'` | Activate the venv and run `pip install pyserial` (the package is `pyserial`, not `serial`). |
| Red dot in the browser | `oracle.py` isn't running, or the page was opened as a file instead of via `http://localhost:8000`. |
| Stuck on "thinking" | The ESP32 may still be connecting to WiFi or the API call failed. Check the terminal; `oracle.py` prints all ESP32 output as `ESP32 \| ...`. Press the board's reset button. |
| `API response code: 429` | Rate limited. Wait and retry. |
| `API response code: 404` | Model ID not available for your key. Change it in `askGemini()`. |
| Solenoids don't move | Check the external power supply, driver wiring, common ground, and GPIO numbers. |
| ESP32 resets when `oracle.py` starts | Expected: opening the serial port resets the board. The script waits 2 seconds for it. |

## Security note

Do not commit or share the `.ino` with real WiFi credentials or API keys in it. If a key has ever been shared or pushed anywhere, revoke it in Google AI Studio and create a new one.
