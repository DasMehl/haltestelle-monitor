# Troubleshooting

Practical fixes for the issues people actually hit setting this up.

## Flashing / Upload

### `A fatal error occurred: Failed to connect to ESP32: Wrong boot mode detected (0x13)!`

The board isn't in bootloader (download) mode. Some CH340-based boards (including the
common "CYD" ESP32-2432S028 this project targets) don't reliably auto-reset into
download mode over USB on Windows.

Fix:

1. Press and hold the **BOOT** button on the board — keep holding it.
2. Start the upload (`platformio run --target upload`).
3. Keep holding **BOOT** through the whole `Connecting.......` phase, until you see
   `Writing at 0x...` progress output start.
4. Only then release **BOOT**.

The key part people get wrong: pressing BOOT *before* starting the upload command and
letting go before it actually connects doesn't work — the board will have already
rebooted normally by the time esptool tries to talk to it. Hold it continuously through
the connection attempt itself.

If that still fails, power-cycle while holding BOOT (unplug USB, hold BOOT, plug USB back
in, keep holding ~2 more seconds, then release) before retrying the upload.

### Upload works but the board only shows a black screen afterward

Give it a few seconds — on first boot after a fresh flash, the firmware shows Wi-Fi/stop
setup status screens (see [GETTING_STARTED.md](./GETTING_STARTED.md)) before the normal
departure UI appears.

## Wi-Fi Setup

### I can't find the `HaltestelleMonitor-Setup` Wi-Fi network on my phone

- Make sure you actually triggered setup mode: hold the touchscreen down, press
  RESET/EN while still holding it, and keep holding for ~2 seconds after the reset.
- The access point only stays up for 10 minutes before giving up — if you waited too
  long between triggering it and checking your phone, it may have already timed out.
  Redo the touch+reset step.
- Manually refresh/rescan Wi-Fi on your phone (toggle Wi-Fi off and back on) — phones
  sometimes cache a stale scan and won't show a newly-appeared network until rescanned.

### The setup portal times out before I finish

The portal window is 10 minutes. If you need longer, just redo the touch+reset step to
reopen it.

## Haltestelle (Stop) Selection

### I can't reach `http://haltestelle.local`

mDNS `.local` names don't resolve reliably from every phone (this is a known issue on
some Android devices in particular). Use the IP address shown on the device screen
instead, e.g. `http://192.168.1.42/` — this always works as long as your phone is on the
same Wi-Fi network as the device.

### The page just shows "Fehler" and nothing else

This means a request reached the device with missing/invalid data (e.g. you navigated
directly to `/set` instead of the root page). Go back to `http://<device-ip>/` and use
the search box + result list instead of typing a URL by hand.

### I searched for a stop and got no results, or the wrong stop

VRR's stop search matches names across all of Germany, not just your area — try a more
specific query (include the city name, e.g. "Duesseldorf Steinstr" instead of just
"Steinstr") to narrow it down, then pick the correct one from the result list.

### I picked a new stop but the departures shown are still from the old one

This should now resolve itself within a few seconds automatically — the page shows
"Wird aktualisiert..." and then "Gespeichert und aktualisiert!" once the on-device
background refresh completes. If it instead says "Update dauert länger als erwartet"
after ~7 seconds, the device likely hit a temporary network hiccup; it will keep
retrying on its own, or you can just try selecting the stop again.

## General

### The screen freezes for a few seconds sometimes

Departure data refreshes happen in the background and shouldn't block the UI. If you
see a freeze during normal operation (not during setup), that's a regression — please
report it with what you were doing right before it happened.

### The status chip says "vor X Min" instead of "Live"

The last successful data fetch is X minutes old. The device keeps counting departures
down locally in the meantime (and drops trains off the list once they've departed), so
the display stays useful. It also heals itself: after 5 minutes without a successful
update it forces a Wi-Fi reconnect, and after 10 minutes it restarts entirely — the
same thing a manual power cycle would do. If you see it restart on its own now and
then, that's this watchdog doing its job; if it happens constantly, check whether the
VRR endpoint is reachable from your network at all.

### The device restarts by itself every 10 minutes

That's the freshness watchdog (see above) firing repeatedly, which means no fetch ever
succeeds. Likely causes: the network blocks plain-HTTP traffic (some guest/corporate
networks do), or the VRR open-data endpoint is down. Check the serial monitor output
for the exact fetch errors.
