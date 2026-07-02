# Wiring Notes

This project is configured for an ESP32 with:

- an ILI9341-compatible `320x240` TFT
- an XPT2046 touch controller

## TFT Display Pins

These pins come from [platformio.ini](../platformio.ini) through `TFT_eSPI` build flags:

| Signal | ESP32 Pin |
| --- | --- |
| `TFT_MISO` | `GPIO12` |
| `TFT_MOSI` | `GPIO13` |
| `TFT_SCLK` | `GPIO14` |
| `TFT_CS` | `GPIO15` |
| `TFT_DC` | `GPIO2` |
| `TFT_RST` | `-1` |
| `TFT_BL` | `GPIO21` |

## Touch Controller Pins

These pins are defined in [src/main.cpp](../src/main.cpp):

| Signal | ESP32 Pin |
| --- | --- |
| `TOUCH_CS` | `GPIO33` |
| `TOUCH_IRQ` | `GPIO36` |
| `TOUCH_MOSI` | `GPIO32` |
| `TOUCH_MISO` | `GPIO39` |
| `TOUCH_SCLK` | `GPIO25` |

## Notes

- The current firmware uses a dedicated `VSPI` instance for the touch controller.
- The LCD backlight is enabled from firmware by driving `GPIO21` high in `setup()`.
- Display rotation is set to landscape mode with `tft.setRotation(3)`.
- Touch coordinates are calibrated in software inside `readTouchPoint()`.

## Before Rewiring

If your board differs from the current setup, update:

- display pin flags in [platformio.ini](../platformio.ini)
- touch pin constants in [src/main.cpp](../src/main.cpp)
- touch calibration values in `readTouchPoint()` if necessary
