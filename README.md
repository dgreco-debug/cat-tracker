# Cat Tracker

A small LoRa GPS tracker for a cat collar, a base station that hears it, and a web app to watch it on a map,
change its settings and chat between bases.

```
 collar (XIAO nRF52840 + SX1262 + M10 GPS)
     |  LoRa 915 MHz, reports every few minutes, listens briefly for commands
 base station (Heltec WiFi LoRa 32 V4)
     |  Bluetooth or USB
 web app (Chrome or Edge on Android, Windows, Mac)
```

The app can also connect to the collar directly over Bluetooth when it is close by.

## Hardware

- Collar: Seeed XIAO nRF52840 + Wio-SX1262 for XIAO kit, SEQURE M10-12 GPS (u-blox M10), small LiPo, 915 MHz flex antenna.
  GPS wiring: GPS RX to D6, GPS TX to D7, V to 3V3, GND to GND.
- Base: Heltec WiFi LoRa 32 V4 with its antenna.

## Getting started

1. **Collar.** Plug the XIAO into USB, double-tap its RST button, and copy `firmware/bin/cat_collar.uf2` onto the
   drive that appears. It reboots by itself.
2. **Base.** Open Espressif's web flasher (esptool-js) in Chrome, connect to the Heltec, and flash
   `firmware/bin/cat_base_heltec_v4.bin` at address `0x0`. Press RST afterwards.
3. **App.** Open https://dgreco-debug.github.io/cat-tracker/ in Chrome (Android or desktop), choose Connect, then
   Bluetooth, and pick `CatBase-xxxx`. On Android you can add it to the home screen. Choose Demo to try the app without hardware.

The collar is visible over Bluetooth for a few minutes after power-up or a press of the user button on the
Wio-SX1262 board, and the whole time it is on USB power.

## Using the app

- **Map:** the collar's position (amber eyes), accuracy circle and trail, your own position, and the distance and
  direction to the cat. Street or satellite view. "Walk there in Google Maps" opens walking directions.
- **Collar:** battery, last check-in, GPS quality, signal, quick actions (find mode, locate now, go quiet), battery
  graph, and all settings with a battery-life estimate.
- **Chat:** short messages to other bases on the same network, with delivery confirmation.
- **Activity:** every check-in, command and message; download the track as CSV.

Commands sent through the base reach the collar right after its next check-in, so they can take up to one
interval. With the collar connected over Bluetooth, changes apply immediately.

## Before it goes on the cat

1. The watchdog is off (`USE_WATCHDOG 0` in `firmware/cat_collar/main.cpp`). Turn it on so the collar recovers from
   a hang by itself. The nRF52 watchdog keeps running after a firmware update, so any other firmware flashed later
   resets every 3 minutes until the board is power-cycled.
2. The GPS is wired straight to 3V3 and only put into software standby between fixes. Measure the sleep current; a
   power LED on the GPS board will drain the battery unless removed. A P-channel MOSFET on 3V3 (gate on a free pin)
   would cut GPS power completely.
3. A collar told to go quiet cannot be reached until it wakes up.
4. Chat messages are limited to 112 bytes and go out as 14-byte radio packets (a full message takes about 3 seconds
   of airtime).

## Repository

- `app/`: web app (Vite, TypeScript, Preact, Leaflet). `npm install`, `npm run dev`.
- `firmware/common/`: shared radio protocol and config helpers (copied into each sketch folder).
- `firmware/cat_collar/`, `firmware/cat_base/`: Arduino sketches. See `firmware/README.md`.
- `firmware/bin/`: ready-to-flash builds.
- `PROTOCOL.md`: radio packets and the JSON messages the app uses.

Map data &copy; OpenStreetMap contributors; satellite imagery &copy; Esri.
