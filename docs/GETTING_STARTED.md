# Getting Started

A complete walkthrough from a fresh ESP32 to a working departure monitor, assuming no
prior context.

## What You Need

- An ESP32 dev board with a built-in 320x240 ILI9341 TFT and XPT2046 touch controller
  (this project targets the common "CYD" ESP32-2432S028 boards specifically, but any
  board wired per [WIRING.md](./WIRING.md) will work)
- A USB cable
- A computer with [PlatformIO Core](https://platformio.org/install/cli) installed
  (`pip install platformio`, or use the PlatformIO IDE extension for VS Code)
- A phone or laptop on the same Wi-Fi network you want the device to join

## 1. Build And Flash

```bash
platformio run
```

Then flash it:

```bash
platformio run --target upload --upload-port <YOUR_PORT>
```

Replace `<YOUR_PORT>` with your board's serial port (e.g. `COM5` on Windows,
`/dev/cu.usbserial-XXXX` on macOS, `/dev/ttyUSB0` on Linux).

**If the upload fails with `Wrong boot mode detected`:** your board needs a manual
push into bootloader mode. Hold the **BOOT** button down, start the upload command,
and keep holding BOOT through the entire `Connecting.......` phase until you see
`Writing at 0x...` appear — then release. See
[TROUBLESHOOTING.md](./TROUBLESHOOTING.md) if it still doesn't work.

## 2. First Boot — Wi-Fi Setup

On first boot, the screen shows **"Schritt 1: WLAN — Nicht verbunden"** and the device
opens its own Wi-Fi access point called `HaltestelleMonitor-Setup`.

1. On your phone, connect to the `HaltestelleMonitor-Setup` Wi-Fi network.
2. A setup page should open automatically (or open a browser and go to `192.168.4.1`
   if it doesn't).
3. Pick your home Wi-Fi network from the list and enter its password, then submit.

The device saves this and reconnects — the screen switches to **"Schritt 2:
Haltestelle — WLAN verbunden"**.

## 3. First Boot — Choose Your Stops

Once Wi-Fi is connected, the screen shows an address to visit, e.g.
`http://192.168.1.42/` (use the IP address shown, not the `.local` name — see
[TROUBLESHOOTING.md](./TROUBLESHOOTING.md) for why).

1. On your phone or laptop (same Wi-Fi network as the device), open that address in a
   browser.
2. You'll see two sections: **Straßenbahn** and **U-Bahn**. These are independent —
   most physical locations only have one or the other, so you may only need to fill in
   one section.
3. In a section, type part of a stop name (e.g. `Steinstr` or `Duesseldorf Hauptbahnhof`)
   and tap **Suchen**.
4. A list of matching stops appears — tap the correct one to save it.
5. The page confirms once the device has fetched live data for that stop.

That's it — the device now shows live departures. This page stays reachable any time
the device is on Wi-Fi, so you can come back later to change either stop.

## Day-to-Day Use

- **Tap the top tab area** to switch between Straßenbahn and U-Bahn.
- **Tap the departure list** to switch direction (the two directions of travel for
  the currently selected stop).
- Departure countdowns tick down on their own between refreshes and correct themselves
  once new live data arrives.

## Changing Wi-Fi Or A Stop Later

- **To change Wi-Fi:** hold the touchscreen down, press the physical RESET/EN button
  while still holding the screen, and keep holding for ~2 seconds after the reset.
  This wipes the saved network and reopens the `HaltestelleMonitor-Setup` access point.
- **To change a stop:** just revisit `http://<device-ip>/` (shown once, right after
  Wi-Fi connects at boot — check your router's connected-devices list, or the serial
  monitor output, if you didn't note it down) and search again.

## Something Not Working?

See [TROUBLESHOOTING.md](./TROUBLESHOOTING.md) for fixes to the issues people actually
run into (flashing failures, the setup AP not showing up, mDNS not resolving, stops not
updating, and more).
