# ESP32-S3 VOM 703S Dashboard

This project turns an ESP32-S3 into a UART-to-Web bridge for a Zoyi 703S multimeter.

## Wiring
- Meter TX -> ESP32 GPIO44 (U0RXD) on the ESP32-S3 DevKitC-1
- Meter GND -> ESP32 GND
- Do not connect ESP32 TX to the meter unless you have a specific two-way protocol need; this project is read-only from the meter.

Current hardware status in your setup:
- UART connected: meter TX to ESP32 GPIO44 (RX)
- GND connected: meter GND to ESP32 GND

## Notes
- The meter output is assumed to be a simple UART text stream, usually 9600 baud.
- The firmware creates an AP named `VOM-703S` and serves a neon-style dashboard at `http://192.168.4.1/`.
- WebSocket stream is available at `ws://192.168.4.1:81/`.
- The PC app connects to the same socket and prints live values.

## Quick start
1. Copy this folder into a PlatformIO project.
2. In VS Code, install the PlatformIO extension.
3. Open the folder and click `Upload`.
4. Connect to Wi‑Fi SSID `VOM-703S` with password `zoyi703s123`.
5. Open `http://192.168.4.1/` in a browser.
6. Run the Python example: `python pc_app/app_client.py`.

## COM3 note
The ESP32 is not on COM3 in the firmware; COM3 is the USB serial port on your PC when you upload firmware or view logs via the serial monitor. The ESP32 itself exposes the dashboard over Wi‑Fi, not over a COM port.
