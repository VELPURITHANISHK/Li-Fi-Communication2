# Li-Fi Communication v2

Send any text (letters, numbers, symbols, spaces, emoji) from a phone flashlight or screen to an Arduino Uno with an LDR module and a 16x2 I2C LCD.

## Files
```
index.html / style.css / app.js   Web transmitter (deploy to Netlify, open on phone)
arduino_receiver.ino              Arduino Uno receiver sketch
sw.js, manifest.json, ...         PWA / hosting files
```

## Wiring
| LDR module | Arduino | | LCD (I2C) | Arduino |
|---|---|---|---|---|
| VCC | 5V | | VCC | 5V |
| GND | GND | | GND | GND |
| DO  | D2 | | SDA / SCL | A4 / A5 |

Library needed: **LiquidCrystal I2C** (Frank de Brabander). The LCD address (0x27 / 0x3F ...) is detected automatically.

## First-time setup (do this once!)
1. In `arduino_receiver.ino` set `#define CALIBRATE 1`, upload, open Serial Monitor (9600).
2. In normal room light the LCD must say **DARK**. Hold the phone flashlight 2-5 cm from the LDR: it must say **LIGHT**. Turn the blue potentiometer on the LDR module until both are true. Dim the room if needed.
3. If it is inverted (LIGHT when dark) change `LIGHT_LEVEL` from `LOW` to `HIGH`.
4. Set `CALIBRATE 0` and upload again.

## Sending
1. Open the website on your phone (HTTPS, e.g. Netlify). Use Chrome on Android for the flashlight; iPhone/others use **Screen flash** automatically.
2. Type the message (max 64 bytes), choose a speed, press **Transmit Light**, allow camera (needed to control the torch).
3. You have 3 seconds to put the phone light 2-5 cm from the LDR. Keep it steady and keep the screen on until it finishes.
4. The LCD shows the message only if the CRC check passes. Otherwise it shows the reason (`Bad start`, `Lost sync`, `CRC error`...) and the Serial Monitor prints details.

## Speed
Fast 100 ms, Normal 150 ms, Safe 250 ms per half-bit. The receiver measures the speed from the start header, so no code change is needed. If you get errors, use Safe, reduce ambient light, and move the phone closer.

## Protocol
`HEADER (light ON 8T) | GAP (OFF 2T) | 0xA5 | LEN | payload | CRC8` - Manchester coded (bit 1 = ON then OFF, bit 0 = OFF then ON). CRC-8 poly 0x07.
