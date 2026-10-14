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
- Battery: 100 mAh LiPo (changed from the 400 mAh 802030 originally planned). Treated as empty at 3.0 V. Battery sense on PIN_VBAT (P0.31) with VBAT_ENABLE (P0.14) held LOW.
- Base: Heltec V4 pins come from Meshtastic's `variants/esp32s3/heltec_v4` (LoRa CS 8, SCK 9, MOSI 10, MISO 11,
  RST 12, BUSY 13, DIO1 14; FEM power 7, CSD 2, GC1109 CPS 46, KCT8103L CTX 5; OLED SDA 17, SCL 18, RST 21,
  Vext 36 active low; button 0; LED 35).

## State at handoff

| Item | State |
|---|---|
| Repo | `dgreco-debug/cat-tracker` (public), branch `main` |
| App | Deployed to GitHub Pages by `.github/workflows/pages.yml` on pushes to `app/**`. Tested only in demo mode. |
| Base Heltec #1 | Running base v2. Name `Base-a51b` (Bluetooth `CatBase-a51b`), front end detected as KCT8103L. Opening its USB serial port resets it (even with DTR/RTS off), which wipes the in-memory history. |
| Base Heltec #2 | Still running Meshtastic 2.7.26 (OLED build). Needs base v2 for chat. |
| Collar | Running **v2.2** (temperature; flashed 2026-10-14 via the `dfu` serial command; bootloader UF2 0.6.1, S140 7.3.0). Name `CatCollar-4345`. Interval 30 s from testing; set back to 300. |
| Base firmware | Base #1 runs **2.3** (flashed over USB with esptool on COM6): new OLED screen, home location, signal %. Home was set from an average of indoor collar fixes (about +/-30 m); reset it with the collar beside the base outdoors (hold PRG 2 s). |
| Last real hardware result | v1 collar: GPS found at 115200, first fix about +/-55 m outdoors-ish, LoRa transmit OK. Indoors only 3 to 4 satellites. |

### Verified

- Both firmwares compile (Seeed nRF52 core 1.1.13; arduino-esp32 3.3.12; RadioLib 7.8.1; SparkFun u-blox GNSS v3
  3.1.15; ArduinoJson 7.4.3; U8g2 2.36.19, the newest in the library index; an earlier note said 2.37.1, which does
  not exist there). 2.1 built with arduino-cli 1.5.1; the nRF52 build needs `python` on PATH.
- Airtime at SF10/125k: 24-byte report 371 ms, 9-byte command 248 ms (all packets kept at 24 bytes or less).
- App: type-checks and builds; demo mode exercised end to end in headless Chromium (map, settings send, find mode,
  chat with delivery ack, activity log) with no runtime errors.
- Hardware, 2026-10-14: collar v2 boots with `Radio: ok` and `GPS: found at 115200`; base v2 receives its reports
  (fix +/-18 m, 5 sats, 29 s); `now` and a config change both went sent -> done in the same check-in, and the
  collar returned the new config. RSSI read 0 dBm with the boards side by side (receiver overload; recheck at range).
- App battery display: the settings line is a power-model prediction for a 100 mAh cell; the "days left" tile is
  measured: a line fitted to state of charge over at least 10 check-ins spanning 30 min or more, on battery and at
  one check-in interval (it restarts after a charge or an interval change), projected to 3.0 V.
- Field failure on 2.0, 2026-10-14: after about 2.5 h, much of it on the 100 mAh battery at a 15 s interval and
  22 dBm, every transmit returned -5 (RADIOLIB_ERR_TX_TIMEOUT) while GPS kept working; it stayed stuck on USB power
  and cleared on restart. Likely a supply dip resetting the SX1262 (losing TCXO and packet settings) while the MCU
  ran on. 2.1 re-initialises the radio and retries once after a failed transmit, and counts it (`radio_reinits` in
  `status`). Watch that counter in the field; if it climbs, the battery cannot supply the transmit current.
- 2.1 on the collar: boot reason correctly reported as software reset after the UF2 update; no-fix report #0 sent;
  `interval_s` 15 survived the restart (config persistence, item 5, confirmed).
- Temperature (2.2): the collar reads the nRF52840 die sensor at the start of each check-in (before GPS and transmit
  warm it) and sends report type `0x04`, which trades the 2-byte accuracy for 1 byte plus a temperature byte. Base 2.2
  passes `temp_c` to the app (tile on the Collar page, activity log, CSV). Base 2.2 verified with a 2.1 collar; the
  2.2 collar is built but not yet flashed.
- Base screen (2.3): compass needle from the base to the collar, satellite count inside the compass opposite the needle, distance in ft/mi, 8-point direction and position
  age, battery %, signal % and temperature, plus 3 s banners (find queued, command done, chat received, home set).
  Needs "home" (the base location): stored in the base's flash only, never in the repo. Set by holding PRG 2 s
  (collar's last position), the app's "Set base location from collar" button, `{"cmd":"set_home"}`, or USB `home <lat> <lon>`.
- Signal % (base screen and app): min of (RSSI + 125) / 65 and (SNR + 15) / 25, as 0-100.
- Reset reason (2.1): the collar reads `readResetReason()` at boot, logs it, returns `reset`/`reset_code` in
  `status`, and sends it in the boot report's `last_cmd` byte; the base adds `reset` to that report's JSON and the
  app logs "Collar restarted ...". A 2.0 collar always reads as "power-on or brownout" on a 2.1 base.

### Not verified (do these first)

1. Collar v2 boot log: `Radio: ok` and `GPS: found at 115200` seen; `Bluetooth on for 5 min` and `Sent report #0` not
   yet seen on serial (reports did reach the base). The collar rebooted once on its own during testing (report #0 with
   the boot flag); cause unknown, possibly a manual reset.
3. App connects to the base over Web Bluetooth (Chrome on Windows and Android) and receives history.
6. App to collar direct over Bluetooth (`CatCollar-xxxx`): status, config save, `dfu` (restart into UF2 bootloader).
7. Battery reading: still wrong on v2 (4163 mV then 3764 mV on consecutive reports, on USB), so
   `analogSampleTime(40)` did not fix it. Check against a multimeter with a battery connected. The app's "days left"
   countdown depends on this reading.
8. Chat between two bases (needs Heltec #2 on base v2).
9. Sleep current of the collar with the GPS in software standby (target tens of uA plus the GPS module's regulator;
   a power LED on the GPS board would add mA).

### Firmware review (2026-10-14; #1 and #2 fixed in 2.1, the rest open)

1. **Fixed in 2.1.** Collar: if the Bluetooth window expires on battery, then USB is plugged in and removed, advertising never stops
   (`bleService`, `bleWindowOpen` is already false). Drains the battery after every charge.
2. **Fixed in 2.1.** Base: after acknowledging a chat message (`onTextFrag`) the radio is not put back into receive; it only recovers
   because the transmit-done IRQ sets `rxFlag`, and the base then parses its own ack from the radio buffer (it can
   list itself as a node when the last fragment is 3 bytes).
3. Base: chat fragments longer than 24 bytes are not rejected, so `r->len[idx]` can exceed 14 and overflow the
   reassembly buffers.
4. Base: `scanChannel()` before a hello or chat aborts receive, so a collar report on the air at that moment is lost.
5. Base: `nextCmdSeq` is random after reboot and can equal the collar's last applied seq (about 1 in 250), so the
   first command is acked as a duplicate, not applied, and shown as done.
6. Collar: a rejected `interval` command still turns off find mode.
7. Collar: a packet that is not a command restarts a full 1.5 s window and uses one of the 6 command slots.
8. Base: `cancel` clears `awaitingReply` even when the in-flight head item was not cancelled.
9. Collar: `setOutputPower` between check-ins leaves the SX1262 in standby until the next report.

Checked and fine: queue advance and listen-window timing, collar rxFlag clearing after transmit, base ISR
(`CONFIG_ARDUINO_ISR_IRAM` is off so GPIO ISRs are deferred during flash writes), BLE RX stream buffer.

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
- Base: Espressif web flasher (esptool-js) in Chrome, file `cat_base_heltec_v4.bin` at `0x0`. Writing the merged image
  from `0x0` erases the settings partition (base name and home location). To update while keeping them, write only
  the app: `esptool --chip esp32s3 -p COMx write-flash 0x10000 cat_base.ino.bin`. If it will not
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
4. **Default interval and battery.** 5 min default gives roughly 3 to 4 days on the 100 mAh cell with warm GPS starts (app estimate). Different
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
