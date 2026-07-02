# Haltestelle Monitor

ESP32 firmware for a small touchscreen departure monitor that shows live VRR departures for `Steinstr./Koenigsallee`.

## What It Does

- Connects an ESP32-based 320x240 touchscreen display to Wi-Fi
- Fetches live departure data from the VRR departure monitor endpoint
- Shows tram and U-Bahn departures in a simple touch-driven UI
- Displays platform information and a scrolling live hint ticker
- Lets you switch mode and direction by touching the screen

## Hardware / Target

This project is currently configured for an `esp32dev` target with an ILI9341-compatible 320x240 TFT and XPT2046 touch controller.

Configured display and touch pins are defined in:

- [platformio.ini](./platformio.ini)
- [src/main.cpp](./src/main.cpp)

## Project Structure

- [src/main.cpp](./src/main.cpp): main firmware, UI rendering, touch handling, Wi-Fi, and VRR fetching
- [platformio.ini](./platformio.ini): PlatformIO environment, board config, library dependencies, display build flags

## Dependencies

PlatformIO installs these automatically:

- `TFT_eSPI`
- `XPT2046_Touchscreen`
- `ArduinoJson`
- ESP32 Arduino framework libraries such as `WiFi`, `HTTPClient`, and `WiFiClientSecure`

## Build And Flash

1. Install PlatformIO Core or open the project in VS Code with PlatformIO.
2. Connect the ESP32 over USB.
3. Adjust `upload_port` and `monitor_port` in [platformio.ini](./platformio.ini) if your serial device differs.
4. Build:

```bash
platformio run
```

5. Flash:

```bash
platformio run --target upload
```

6. Open serial monitor:

```bash
platformio device monitor
```

## Current Runtime Configuration

The current firmware keeps a few settings directly in source:

- Wi-Fi SSID and password
- stop label and stop ID
- VRR endpoint URL

These are defined near the top of [src/main.cpp](./src/main.cpp).

## Touch Controls

- Tap the upper tab area to switch between `Strassenbahn` and `U-Bahn`
- Tap the departures area to switch direction

## Data Source

The firmware fetches live data from:

`https://openservice-test.vrr.de/static03/XML_DM_REQUEST`

The request is configured with:

- `useRealtime=1`
- stop ID `20018234`
- JSON output

## Known Limitations

- The current refresh logic is synchronous, so the UI can briefly freeze while data is fetched and parsed.
- Wi-Fi credentials are hardcoded in source.
- The project is currently tailored to one specific stop and one specific display setup.

## Next Improvements

- Move Wi-Fi and stop configuration into a separate config file or onboarding flow
- Make live refresh non-blocking so the UI stays responsive during network requests
- Add support for multiple saved stops
- Add screenshots and wiring notes to this repository
