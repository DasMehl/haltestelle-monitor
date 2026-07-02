# Haltestelle Monitor

ESP32 firmware for a small touchscreen departure monitor that shows live VRR departures.

**New here?** Follow [docs/GETTING_STARTED.md](./docs/GETTING_STARTED.md) for a full
walkthrough from a fresh board to a working monitor. Something not working? Check
[docs/TROUBLESHOOTING.md](./docs/TROUBLESHOOTING.md).

## What It Does

- Connects an ESP32-based 320x240 touchscreen display to Wi-Fi, configured entirely on-device (no hardcoded credentials)
- Lets you search VRR's stop database by name and pick your own Straßenbahn and U-Bahn stops from a web page, independently of each other
- Fetches live departure data from the VRR departure monitor endpoint for each configured stop
- Shows tram and U-Bahn departures in a simple touch-driven UI
- Displays platform information and a scrolling live hint ticker
- Lets you switch mode and direction by touching the screen
- Refreshes departure data in the background, so the UI never freezes while fetching
- Counts departure times down locally between refreshes, so minutes keep ticking down to `sofort` even if the network is briefly unavailable

## Hardware / Target

This project is currently configured for an `esp32dev` target with an ILI9341-compatible 320x240 TFT and XPT2046 touch controller.

Configured display and touch pins are defined in:

- [platformio.ini](./platformio.ini)
- [src/main.cpp](./src/main.cpp)
- [docs/WIRING.md](./docs/WIRING.md)

## Project Structure

- [src/main.cpp](./src/main.cpp): main firmware, UI rendering, touch handling, Wi-Fi, and VRR fetching
- [platformio.ini](./platformio.ini): PlatformIO environment, board config, library dependencies, display build flags
- [docs/GETTING_STARTED.md](./docs/GETTING_STARTED.md): full setup walkthrough for new users
- [docs/TROUBLESHOOTING.md](./docs/TROUBLESHOOTING.md): fixes for common setup/flashing issues
- [docs/WIRING.md](./docs/WIRING.md): pin mapping and wiring notes
- [docs/SCREENSHOTS.md](./docs/SCREENSHOTS.md): UI preview and screenshot placeholders

## Dependencies

PlatformIO installs these automatically:

- `TFT_eSPI`
- `XPT2046_Touchscreen`
- `ArduinoJson`
- `WiFiManager`
- ESP32 Arduino framework libraries such as `WiFi`, `HTTPClient`, and `WiFiClientSecure`

## Build, Flash, And Setup

See [docs/GETTING_STARTED.md](./docs/GETTING_STARTED.md) for the full walkthrough,
including the on-device Wi-Fi and Haltestelle (stop) setup wizard. Quick reference:

```bash
platformio run                                        # build
platformio run --target upload --upload-port <PORT>   # flash
platformio device monitor --port <PORT>               # serial monitor
```

If upload fails with `Wrong boot mode detected`, see
[docs/TROUBLESHOOTING.md](./docs/TROUBLESHOOTING.md).

## Touch Controls

- Tap the upper tab area to switch between `Strassenbahn` and `U-Bahn`
- Tap the departures area to switch direction
- Hold the touchscreen while powering on to reopen the Wi-Fi setup portal

## Visual Reference

- [docs/SCREENSHOTS.md](./docs/SCREENSHOTS.md)

## Data Source

The firmware talks to two VRR open-data endpoints:

- `https://openservice-test.vrr.de/static03/XML_DM_REQUEST` — live departures for a configured stop ID, with `useRealtime=1` and JSON output.
- `https://openservice-test.vrr.de/static03/XML_STOPFINDER_REQUEST` — stop name search (used by the Haltestelle web page), returning candidate stop IDs and names for a typed query.

## Known Limitations

- The project is currently tailored to one specific display setup (ILI9341 320x240 + XPT2046 touch).
- Only one Straßenbahn stop and one U-Bahn stop can be configured at a time (no support for saving multiple alternates per mode).

## Next Improvements

- Add support for multiple saved stops per mode, selectable from the touchscreen
- Add real device photos or captured display screenshots to replace the current placeholder preview
