# Protocols

Two layers: the LoRa packets between devices, and the JSON lines between the app and a device.

## LoRa (firmware/common/protocol.h)

915.0 MHz, 125 kHz bandwidth, SF10, coding rate 4/5, private sync word 0x1D, 8-symbol preamble.
Every packet starts with `0xC7` (magic), the network id (`0x42` by default) and a type byte, and is at most
24 bytes so it stays under about 370 ms on air (the US 400 ms dwell limit).

| Type | Dir | Bytes | Contents |
|---|---|---|---|
| `0x01` report | collar to base | 24 | seq, flags, lat/lon (1e-7 deg), accuracy m, satellites, battery mV, GPS seconds, last command seq (reset reason on the boot report), interval s, position age min |
| `0x02` ack | collar to base | 6 | command seq, command code, status (0 applied, 1 duplicate, 2 rejected) |
| `0x03` config | collar to base | 21 | command seq, status, 16-byte config |
| `0x10` command | base to collar | 9 | command seq, code, 32-bit argument |
| `0x11` set config | base to collar | 20 | command seq, 16-byte config |
| `0x20` text | base to base | up to 24 | sender id, message id, fragment index/count, up to 14 bytes of UTF-8 |
| `0x21` text ack | base to base | 13 | acknowledging node, original sender, message id |
| `0x22` hello | base to base | 7 + name | node id, name up to 16 bytes |

Report flags: `0x01` fresh fix, `0x02` low battery, `0x04` find mode, `0x08` GPS not responding, `0x10` on USB power,
`0x20` first report after power-up, `0x40` collar Bluetooth on.

On the first report after power-up (flag `0x20`) the last-command byte carries why the collar restarted instead:
`0x01` reset button, `0x02` watchdog, `0x04` software reset (firmware restart or crash handler), `0x08` CPU lockup,
`0x10` wake from sleep, `0x20` USB power connected; `0` means power-on or brownout.

Command codes: `1` interval (s), `2` sleep (min), `3` find (min), `4` transmit power (dBm), `5` report now, `6` read config.

After each report the collar listens for `rx_window_ms`. Each command it answers opens another 1.5 s window,
so the base can deliver several queued items back to back. Commands carry a sequence number; a repeat of the last
applied one is acknowledged as a duplicate and not applied twice.

## JSON lines (app to device)

Same format over Bluetooth LE (Nordic UART service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, write to `...0002`,
notifications on `...0003`) and over USB serial at 115200 baud. One JSON object per line. Device output that does not
start with `{` is human-readable logging and can be ignored.

### Base station

App sends:

| `cmd` | Fields | Effect |
|---|---|---|
| `hello` | | replies `hello`, then `queue` |
| `history` | | replays stored reports (`report` with `hist: true`), nodes, last collar config, then `history_end` |
| `collar` | `action`: `find`, `sleep`, `now`, `interval`, `power`, `get_config`; `arg` | queues a command for the collar |
| `collar_config` | `config`: any subset of the config fields | queues a full config update (merged with the last known config) |
| `cancel` | `qid` (0 = all) | drops queued items |
| `queue` | | replies `queue` |
| `send_text` | `text`, `cid` (app's id for the message) | sends a chat message |
| `set_name` | `name` | renames this base (saved) and announces it |
| `nodes` | | replies one `node` per known base |

Base sends: `hello` (fw, id, name, radio settings, `collar_config`), `report`, `cmd` (`qid`, `state`: queued, sent,
waiting, done, rejected, cancelled), `queue`, `collar_config`, `text`, `text_sent`, `text_ack`, `text_unconfirmed`,
`node`, `log`, `error`. A `report` with the boot flag also has `reset` (why the collar restarted, as text). Times are given as `ago_ms` (how long before the message was sent).

### Collar (direct)

App sends `hello`, `status`, `get_config`, `set_config` (`config`: subset), `reset_config`,
`action` (`action`: `now`, `find`, `sleep`; `arg`). The collar replies with `hello`/`status`
(radio, GPS, battery, last fix, config, `reset`/`reset_code`: why it last restarted, and `radio_reinits`: how often the radio was set up again after a failed transmit), `config` (with `saved: true` after a change), `action`, `log`,
`error`, and a `report` (with `local: true`) after each check-in while connected.

### Config fields

| Field | Range | Default |
|---|---|---|
| `interval_s` | 15 to 86400 | 300 |
| `find_interval_s` | 10 to 600 | 30 |
| `fix_timeout_s` | 10 to 250 | 60 |
| `first_fix_timeout_s` | 30 to 250 | 120 |
| `good_hacc_m` | 3 to 100 | 20 |
| `rx_window_ms` | 500 to 5000 | 2000 |
| `tx_dbm` | -9 to 22 | 22 |
| `ble_minutes` | 1 to 60 | 5 |
| `low_batt_mv` | 3000 to 4000 | 3500 |
| `leds` | true/false | true |
