# Handoff: Cat Tracker

Written 2026-10-13 at the end of the first build session, for whoever (Claude Code or a person) picks this up next.
Read this, then `README.md` and `PROTOCOL.md`.

## What this is

A LoRa GPS tracker on a cat collar, a base station at home that hears it, and a web app (phone or PC) that shows
the cat on a map, changes collar settings, logs activity, and chats between bases.

```
collar  XIAO nRF52840 + Wio-SX1262 + SEQURE M10-12 GPS (u-blox M10)
   |    LoRa 915.0 MHz SF10/125k, custom packets (PROTOCOL.md), report every N s, listen 2 s for commands
base    Heltec WiFi LoRa 32 V4 (ESP32-S3, SX1262, GC1109 or KCT8103L front end, OLED)
   |    Bluetooth LE Nordic UART service or USB serial, newline-delimited JSON
app     Vite + TypeScript + Preact + Leaflet PWA on GitHub Pages
        https://dgreco-debug.github.io/cat-tracker/   (Chrome/Edge on Android, Windows, Mac)
```

The app can also talk to the collar directly over Bluetooth (collar advertises for `ble_minutes` after boot or a
press of the Wio-SX1262 user button on D0, and always while on USB power).

## Hardware and wiring

- Collar: Seeed XIAO nRF52840 & Wio-SX1262 kit (stacked, Seeed "kit default" pinout: CS D4, DIO1 D1, RESET D2,
  BUSY D3, RXEN D5, SPI D8/D9/D10, TCXO 1.8 V, DIO2 as RF switch). Flat 915 MHz FPC antenna on the Wio board.
- GPS: SEQURE M10-12 (u-blox M10, ships at 115200 baud UBX at 10 Hz for drones). GPS RX <- D6, GPS TX -> D7,
  V -> 3V3 (straight, no power switch), GND. Firmware autobauds and turns off the drone message spam.
- Battery: 400 mAh LiPo planned (802030). Battery sense on PIN_VBAT (P0.31) with VBAT_ENABLE (P0.14) held LOW.
- Base: Heltec V4 pins come from Meshtastic's `variants/esp32s3/heltec_v4` (LoRa CS 8, SCK 9, MOSI 10, MISO 11,
  RST 12, BUSY 13, DIO1 14; FEM power 7, CSD 2, GC1109 CPS 46, KCT8103L CTX 5; OLED SDA 17, SCL 18, RST 21,
  Vext 36 active low; button 0; LED 35).

## State at handoff

| Item | State |
|---|---|
| Repo | `dgreco-debug/cat-tracker` (public), branch `main` |
| App | Deployed to GitHub Pages by `.github/workflows/pages.yml` on pushes to `app/**`. Tested only in demo mode. |
| Base Heltec #1 | Flashed with base v2 (`firmware/bin/cat_base_heltec_v4.bin` at 0x0, hash verified). User reports it boots. Bluetooth connection from the app not yet confirmed. |
| Base Heltec #2 | Still running Meshtastic 2.7.26 (OLED build). Needs base v2 for chat. |
| Collar | Still running **v1** firmware (no Bluetooth, older packet set). v2 UF2 is built (`firmware/bin/cat_collar.uf2`, also in the user's Downloads as `cat_collar_v2.uf2`) but not flashed: the double-tap of RST to get the XIAO-SENSE drive did not take on the last attempt. |
| Last real hardware result | v1 collar: GPS found at 115200, first fix about +/-55 m outdoors-ish, LoRa transmit OK. Indoors only 3 to 4 satellites. |

### Verified

- Both firmwares compile (Seeed nRF52 core 1.1.13; arduino-esp32 3.3.12; RadioLib 7.8.1; SparkFun u-blox GNSS v3
  3.1.15; ArduinoJson 7.4.3; U8g2 2.37.1).
- Airtime at SF10/125k: 24-byte report 371 ms, 9-byte command 248 ms (all packets kept at 24 bytes or less).
- App: type-checks and builds; demo mode exercised end to end in headless Chromium (map, settings send, find mode,
  chat with delivery ack, activity log) with no runtime errors.

### Not verified (do these first)

1. Flash collar v2 and confirm over USB serial: `Radio: ok`, `GPS: found at 115200`, `Bluetooth on for 5 min`,
   `Sent report #0`.
2. Base hears collar v2 reports (OLED shows `#seq` and position; app gets `report` events).
3. App connects to the base over Web Bluetooth (Chrome on Windows and Android) and receives history.
4. Command round trip over LoRa: queue `find 10` in the app, see `sent` then `done` after the next check-in.
5. Config round trip: change a setting in the app, see `collar_config` come back; reboot the collar and confirm it
   persisted (LittleFS file `/collar_cfg.bin`).
6. App to collar direct over Bluetooth (`CatCollar-xxxx`): status, config save, `dfu` (restart into UF2 bootloader).
7. Battery reading: v1 showed 4.12 V then 3.73 V a minute apart on USB. v2 adds `analogSampleTime(40)` for the
   1M/510k divider. Check against a multimeter with a battery connected.
8. Chat between two bases (needs Heltec #2 on base v2).
9. Sleep current of the collar with the GPS in software standby (target tens of uA plus the GPS module's regulator;
   a power LED on the GPS board would add mA).

A full code review of the v2 firmware was planned but not run. Areas worth a careful look: base command queue
advance and the collar's follow-up listen windows (`MORE_CMD_WINDOW_MS`), RadioLib IRQ flag handling across
transmit/receive on both sides, NimBLE callbacks on the base (they run on another task; RX goes through a FreeRTOS
stream buffer), Bluefruit advertising start/stop on the collar, and chat fragment reassembly.

## Building

Arduino IDE 2 or arduino-cli. Code is in each sketch's `main.cpp`; the `.ino` files are intentionally empty so the
Arduino preprocessor does not rewrite prototypes. `firmware/common/` is the source of truth for `protocol.h` and
`config_json.h`; run `firmware/sync_common.sh` after editing them.

- Collar: board package "Seeed nRF52 Boards" (not mbed), FQBN `Seeeduino:nrf52:xiaonRF52840`.
  UF2: `uf2conv.py -f 0xADA52840 -c -o cat_collar.uf2 cat_collar.ino.hex` (the core's own UF2 step only runs for
  the T1000-E). Bootloader on the board: UF2 0.6.1, SoftDevice S140 7.3.0.
- Base: board package "esp32" 3.3.x, FQBN `esp32:esp32:heltec_wifi_lora_32_V4:CDCOnBoot=cdc,USBMode=hwcdc`.
  Single image for web flashing: `esptool --chip esp32s3 merge-bin -o cat_base_heltec_v4.bin --flash-mode keep
  --flash-freq keep --flash-size keep 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 app.bin`.
- App: `cd app && npm install && npm run dev` (demo mode works without hardware). `npm run build` type-checks.

## Flashing

- Collar: double-tap RST quickly, copy `cat_collar.uf2` to the XIAO-SENSE drive; it reboots by itself (the copy
  tool may report an error because the drive vanishes; that is normal). With v2 running, the app's Collar tab has
  "Restart into update mode" (JSON `{"cmd":"dfu"}`, or type `dfu` on USB serial) so no button taps are needed.
- Base: Espressif web flasher (esptool-js) in Chrome, file `cat_base_heltec_v4.bin` at `0x0`. If it will not
  connect: hold PRG, tap RST, release PRG.

## Things learned the hard way

- `NAME_MAX` is a POSIX macro on ESP32; the protocol constant is `NODE_NAME_MAX`.
- Seeed's variant file says `LED_STATE_ON 1`, but the XIAO RGB LED is common anode (LOW = on).
- Meshtastic's flasher "Meshtastic UI" toggle installs the TFT/touch build meant for Heltec's expansion kit; on a
  bare V4 with the OLED it leaves the screen dead. Keep it off.
- The M10 module is powered from 3V3 permanently, so the collar uses UBX-RXM-PMREQ (indefinite, wake on UART RX)
  between fixes and holds D6 high while the GPS sleeps so noise does not wake it.
- Web Bluetooth does not work in iPhone Safari.

## Open questions for Devon (do not decide these silently)

1. **Watchdog.** `USE_WATCHDOG` is 0 on the collar. Turning it on adds hang recovery but the nRF52 WDT survives
   a firmware update (any other firmware flashed later resets every 180 s until power-cycled). Enable before it
   goes on the cat?
2. **GPS power switch.** Keep software standby, or add a P-channel MOSFET on 3V3 (gate on a free pin) to cut GPS
   power completely (loses hot-start data unless the module has a backup cell)?
3. **Instant wake.** Commands reach the collar only after its next check-in. Add SX1262 duty-cycled receive so the
   base can wake it any time (costs some battery and needs long-preamble packets from the base)?
4. **Default interval and battery.** 5 min default gives roughly 3 weeks on 400 mAh with warm GPS starts. Different
   default, or a bigger battery?
5. **Multiple collars.** Reports carry no collar id (NET_ID only). Add one if a second cat or collar is planned?
6. **Radio plan.** Single fixed channel at 915.0 MHz, SF10, 125 kHz, packets under 400 ms, collar at 22 dBm, base
   about 21 to 23 dBm after the V4 amplifier. Confirm this is acceptable for his use, or change frequency/NET_ID.
7. **Second Heltec.** Move it from Meshtastic to base v2 so chat works?
8. **iPhone.** Support via the Bluefy browser, or a native app later?
9. **Maps.** Currently OpenStreetMap and Esri satellite tiles, plus a "Walk there in Google Maps" link. Embedded
   Google Maps would need an API key with billing.
10. **License.** The repo is public with no license file. Add one?
11. **Privacy.** The track is stored only in the browser (localStorage) and the public site holds no data. Keep it
    that way, or add cloud sync (for example the base forwarding over Wi-Fi)?

## File map

```
app/                      web app (src/link.ts transports, src/store.ts state + protocol handling,
                          src/demo.ts simulated devices, src/components/* views)
firmware/common/          protocol.h, config_json.h (shared)
firmware/cat_collar/      collar sketch (main.cpp)
firmware/cat_base/        base sketch (main.cpp)
firmware/bin/             ready-to-flash builds
firmware/sync_common.sh   copy shared headers into both sketches
PROTOCOL.md               LoRa packets and JSON messages
.github/workflows/        GitHub Pages deploy
```
