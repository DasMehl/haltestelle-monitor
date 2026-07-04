# Haltestelle Monitor

ESP32 firmware for a small touchscreen departure monitor that shows live VRR departures.

**New here?** Follow [docs/GETTING_STARTED.md](./docs/GETTING_STARTED.md) for a full
walkthrough from a fresh board to a working monitor. Something not working? Check
[docs/TROUBLESHOOTING.md](./docs/TROUBLESHOOTING.md).

## What It Does

- Connects an ESP32-based 320x240 touchscreen display to Wi-Fi, configured entirely on-device (no hardcoded credentials)
- Lets you search VRR's stop database by name and pick your own Straßenbahn and U-Bahn stops from a web page, independently of each other
- Fetches live departure data from the VRR departure monitor endpoint for each configured stop, every 30 seconds
- Shows tram and U-Bahn departures in a clean dark UI: proportional FreeSans typography, segmented mode tabs, line-number chips, and a status chip that shows `Live` (green) or `vor X Min` (amber) depending on data freshness
- Displays platform information and a scrolling live hint ticker
- Lets you switch mode and direction by touching the screen
- Refreshes departure data in a background task, so the UI never freezes while fetching
- Counts departure times down locally between refreshes: minutes tick down to `sofort`, and a departed train drops off the list a minute later with the next one shifting up — so the display keeps predicting sensibly even without fresh data
- Heals itself: if no update succeeds for 5 minutes it forces a Wi-Fi reconnect, and after 10 minutes it restarts — so it never sits showing hours-old departures

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
- ESP32 Arduino framework libraries such as `WiFi`, `HTTPClient`, `WebServer`, and `ESPmDNS`

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

<img src="./docs/screenshots/strassenbahn.png" alt="Strassenbahn tab" width="400"> <img src="./docs/screenshots/ubahn.png" alt="U-Bahn tab" width="400">

Rendered to-scale mockups, not live device captures — see
[docs/SCREENSHOTS.md](./docs/SCREENSHOTS.md) for details and to contribute real
device photos.

## Data Source

The firmware talks to two VRR open-data endpoints:

- `http://openservice-test.vrr.de/static03/XML_DM_REQUEST` — live departures for a configured stop ID, with `useRealtime=1`, JSON output, `limit=20`, and a transport-mode filter (`includedMeans` restricted to U-Bahn and tram) so the result slots aren't wasted on buses and trains.
- `http://openservice-test.vrr.de/static03/XML_STOPFINDER_REQUEST` — stop name search (used by the Haltestelle web page), returning candidate stop IDs and names for a typed query.

Requests are made over plain HTTP deliberately: the data is public with no credentials involved, and TLS on the ESP32 proved both fragile for these response sizes and expensive (~45KB of heap per connection). Responses are parsed with an [ArduinoJson filter](https://arduinojson.org/v7/api/json/deserializejson/) so only the handful of fields the display uses are ever kept in memory.

## Known Limitations

- The project is currently tailored to one specific display setup (ILI9341 320x240 + XPT2046 touch).
- Only one Straßenbahn stop and one U-Bahn stop can be configured at a time (no support for saving multiple alternates per mode).

## Next Improvements

- Add support for multiple saved stops per mode, selectable from the touchscreen
- Add real device photos or captured display screenshots to replace the current placeholder preview
