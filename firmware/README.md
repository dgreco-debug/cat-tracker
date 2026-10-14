# Firmware

Both sketches keep their code in `main.cpp`; the `.ino` files are empty on purpose so the Arduino preprocessor
leaves the code alone. `protocol.h` and `config_json.h` are copies of `common/`; after editing them, copy them
into both sketch folders (`./sync_common.sh`).

## Collar (Seeed XIAO nRF52840)

- Arduino board package: **Seeed nRF52 Boards** (not "mbed-enabled"), board **Seeed XIAO nRF52840**.
- Libraries: RadioLib 7.x, SparkFun u-blox GNSS v3, ArduinoJson 7. Bluefruit and LittleFS come with the board package.
- Upload from the IDE, or export a UF2 and copy it to the XIAO's drive after a double-tap of RST.

## Base (Heltec WiFi LoRa 32 V4)

- Arduino board package: **esp32** by Espressif 3.3.x, board **Heltec WiFi LoRa 32(V4)**, USB CDC On Boot: Enabled.
- Libraries: RadioLib 7.x, U8g2, ArduinoJson 7. BLE comes with the board package.
- The V4 has a LoRa front-end amplifier (GC1109 or KCT8103L, detected at boot). `BASE_RADIO_DBM` sets the radio
  chip's output before the amplifier, which adds roughly 10 to 13 dB.

## arduino-cli

```
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 firmware/cat_collar
arduino-cli compile -b "esp32:esp32:heltec_wifi_lora_32_V4:CDCOnBoot=cdc" firmware/cat_base
```
