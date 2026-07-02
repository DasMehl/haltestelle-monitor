# Haltestelle Monitor

ESP32 firmware for a small touchscreen departure monitor that shows live VRR departures.

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
- [docs/WIRING.md](./docs/WIRING.md): pin mapping and wiring notes
- [docs/SCREENSHOTS.md](./docs/SCREENSHOTS.md): UI preview and screenshot placeholders

## Dependencies

PlatformIO installs these automatically:

- `TFT_eSPI`
- `XPT2046_Touchscreen`
- `ArduinoJson`
- `WiFiManager`
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

## Setup Wizard (Wi-Fi + Haltestelle)

Setup is a two-step, on-device wizard. Nothing is hardcoded in source — both steps are gated so step 2 only becomes available once step 1 succeeds, and the TFT shows connection status at each step.

### Step 1 — Wi-Fi

The firmware uses [WiFiManager](https://github.com/tzapu/WiFiManager) to let you pick any network from the device itself:

- On first boot (or if no Wi-Fi is saved), the ESP32 opens a Wi-Fi access point called `HaltestelleMonitor-Setup`.
- Connect to that AP from your phone or laptop; a setup portal lets you pick your network from a scan and enter its password.
- Credentials are saved on-device and reused automatically on future boots.
- To switch to a different network later, hold the touchscreen down while powering on the device — this forces the setup portal to reopen so you can choose a new network.

### Step 2 — Haltestelle (stop) selection

Once Wi-Fi is connected, the firmware starts a small web server on the device for choosing your stops:

- The TFT shows the address to visit once step 1 succeeds.
- **Use the IP address shown on screen (e.g. `http://192.168.x.x`) — the `http://haltestelle.local` mDNS name is shown too, but doesn't reliably resolve from all phones (notably some Android devices), so the IP is the reliable fallback.**
- The page has two independent sections — **Straßenbahn** and **U-Bahn** — since a tram stop and a U-Bahn stop for the same area often have different VRR stop IDs (most locations only have one or the other; having both is a coincidence, not the norm).
- In either section, type part of a stop name and tap "Suchen" to search VRR's stop database; a list of matching stops appears, and tapping one saves it immediately for that mode.
- This page stays available any time the device is connected to Wi-Fi (not just during initial setup), so you can revisit `http://<device-ip>/` any time to change either stop.

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
