// Cat collar tracker v2: Seeed XIAO nRF52840 + Wio-SX1262 for XIAO + u-blox M10 GPS (SEQURE M10-12)
//
// Wiring
//   LoRa:  stacked Wio-SX1262 board (kit pinout): CS D4, DIO1 D1, RESET D2, BUSY D3, RXEN D5, SPI D8/D9/D10
//   GPS:   GPS RX <- D6 (MCU TX), GPS TX -> D7 (MCU RX), V -> 3V3, GND -> GND
//   Button: Wio-SX1262 user button on D0 turns Bluetooth on for config.ble_minutes
//
// Behaviour
//   Every interval: wake GPS, get a fix (or time out), put GPS back to sleep, send a 24-byte report over LoRa,
//   then listen for commands from the base (several can arrive back to back).
//   Settings are saved in flash and can be changed over LoRa (through the base), over Bluetooth, or over USB.
//   Bluetooth (Nordic UART service, JSON lines) is on for a few minutes after boot or a button press,
//   and always while USB power is present.
//
// Code lives here rather than in the .ino so the Arduino builder does not rewrite it.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <RadioLib.h>
#include <SparkFun_u-blox_GNSS_v3.h>
#include <bluefruit.h>
#include <InternalFileSystem.h>
#include <ArduinoJson.h>
#include "protocol.h"
#include "config_json.h"

using namespace cat;
using namespace Adafruit_LittleFS_Namespace;

#define FW_VERSION "cat-collar 2.2"
#define USE_WATCHDOG 0     // set to 1 before the collar goes on the cat (see README)

static const uint32_t MORE_CMD_WINDOW_MS = 1500;  // extra listen window after each command reply
static const uint8_t  MAX_CMDS_PER_CHECKIN = 6;
static const uint32_t MAX_SETTLE_MS = 8000;       // after the first valid fix, wait at most this long for a better one

// ---------------- pins ----------------
static const uint8_t LORA_CS = 4, LORA_DIO1 = 1, LORA_RST = 2, LORA_BUSY = 3, LORA_RXEN = 5;
static const uint8_t PIN_USER_BUTTON = 0;

// ---------------- state ----------------
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
SFE_UBLOX_GNSS_SERIAL gnss;
BLEUart bleuart;

static CollarConfig cfg;
static volatile bool rxFlag = false;
static volatile bool buttonFlag = false;
static bool radioOk = false;
static uint16_t radioReinits = 0;  // radio set up again after a failed transmit (see reportCycle)
static bool gpsOk = false;
static uint32_t gpsBaud = 0;
static bool firstFixDone = false;

static uint32_t findUntilMs = 0;  static bool findActive = false;
static uint32_t nextReportMs = 0;
static uint16_t seq = 0;
static uint8_t lastCmdSeq = 0;    static bool haveCmdSeq = false;
static bool bootReport = true;
static uint8_t resetCode = 0;     // ResetReason for this boot
static uint32_t bleUntilMs = 0;   static bool bleWindowOpen = false;
static char devName[20];
static char devId[9];

struct Fix { bool valid; int32_t lat, lon; uint16_t hacc; uint8_t sats; uint32_t atMs; };
static Fix lastFix = {false, 0, 0, 0, 0, 0};

// ---------------- helpers ----------------
static inline bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }
static bool usbPowered() { return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0; }
static bool bleConnected() { return Bluefruit.connected() > 0 && bleuart.notifyEnabled(); }
static uint32_t currentIntervalS() { return findActive ? cfg.find_interval_s : cfg.interval_s; }

static void sendJsonTo(JsonDocument &doc, bool toUsb, bool toBle) {
  if (toUsb && Serial) { serializeJson(doc, Serial); Serial.println(); }
  if (toBle && bleConnected()) {
    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof buf - 1);
    buf[n++] = '\n';
    bleuart.write((const uint8_t *)buf, n);
  }
}

static void logf(const char *fmt, ...) {
  char buf[200];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (Serial) { Serial.print("["); Serial.print(millis() / 1000); Serial.print("s] "); Serial.println(buf); }
  if (bleConnected()) {
    JsonDocument d; d["evt"] = "log"; d["msg"] = buf;
    sendJsonTo(d, false, true);
  }
}

// XIAO RGB LED is common anode: LOW = on
static void ledsOff() { digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_BLUE, HIGH); }
static void led(uint8_t pin, bool on) { if (cfg.flags & CF_LEDS) digitalWrite(pin, on ? LOW : HIGH); else digitalWrite(pin, HIGH); }
static void blink(uint8_t pin, uint16_t ms) { led(pin, true); delay(ms); led(pin, false); }

#if USE_WATCHDOG
static void wdtStart(uint32_t seconds) {
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) | (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV = 32768UL * seconds - 1;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}
static void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }
#else
static void wdtFeed() {}
#endif

static uint16_t readBatteryMv() {
  analogReference(AR_INTERNAL_2_4);
  analogReadResolution(12);
  analogSampleTime(40);  // the 1M/510k divider needs a long acquisition time to read correctly
  delay(2);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_VBAT);
  float mv = (sum / 8.0f) * 2400.0f / 4096.0f * (1510.0f / 510.0f);
  return (uint16_t)mv;
}

// nRF52840 die temperature in 0.5 degC steps. It tracks the collar's surroundings between check-ins.
static int8_t readTempHalfC() {
  float c = readCPUTemperature();
  if (isnan(c) || c < -60 || c > 120) return TEMP_UNKNOWN;
  long h = lroundf(c * 2);
  return (int8_t)constrain(h, -127L, 127L);
}

// ---------------- config storage ----------------
static const char *CFG_PATH = "/collar_cfg.bin";

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
  return ~c;
}

static void loadConfig() {
  cfg = defaultConfig();
  File f(InternalFS);
  if (!f.open(CFG_PATH, FILE_O_READ)) { logf("Config: defaults"); return; }
  uint8_t buf[sizeof(CollarConfig) + 4];
  int n = f.read(buf, sizeof buf);
  f.close();
  CollarConfig c; uint32_t crc;
  memcpy(&c, buf, sizeof c); memcpy(&crc, buf + sizeof c, 4);
  if (n == (int)sizeof buf && crc == crc32(buf, sizeof c) && c.version == CONFIG_VERSION && configValid(c)) {
    cfg = c; logf("Config: loaded from flash");
  } else {
    logf("Config: stored copy invalid, using defaults");
  }
}

static bool saveConfig() {
  uint8_t buf[sizeof(CollarConfig) + 4];
  memcpy(buf, &cfg, sizeof cfg);
  uint32_t crc = crc32(buf, sizeof cfg);
  memcpy(buf + sizeof cfg, &crc, 4);
  InternalFS.remove(CFG_PATH);
  File f(InternalFS);
  if (!f.open(CFG_PATH, FILE_O_WRITE)) return false;
  bool ok = f.write(buf, sizeof buf) == sizeof buf;
  f.close();
  return ok;
}

// Validates, applies and saves a new config. Returns false (and changes nothing) if out of range.
static bool applyConfig(const CollarConfig &c) {
  if (!configValid(c)) return false;
  bool powerChanged = c.tx_dbm != cfg.tx_dbm;
  bool intervalChanged = c.interval_s != cfg.interval_s;
  cfg = c;
  cfg.version = CONFIG_VERSION;
  if (powerChanged && radioOk) radio.setOutputPower(cfg.tx_dbm);
  if (intervalChanged && !findActive) nextReportMs = millis() + cfg.interval_s * 1000UL;
  if (!(cfg.flags & CF_LEDS)) ledsOff();
  bool saved = saveConfig();
  logf("Config: applied%s (every %lus, %d dBm)", saved ? " and saved" : ", SAVE FAILED",
       (unsigned long)cfg.interval_s, cfg.tx_dbm);
  return true;
}

// ---------------- Bluetooth ----------------
static void bleStartWindow() {
  bleUntilMs = millis() + cfg.ble_minutes * 60000UL;
  bleWindowOpen = true;
  if (!Bluefruit.connected() && !Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.start(0);
  logf("Bluetooth on for %u min (%s)", cfg.ble_minutes, devName);
}

static void bleService() {
  if (buttonFlag) { buttonFlag = false; bleStartWindow(); }
  if (bleWindowOpen && timeReached(bleUntilMs)) bleWindowOpen = false;
  // Advertising may have been started by USB power after the window closed, so stop it whenever neither applies.
  bool keepOn = usbPowered() || bleWindowOpen;
  if (keepOn) {
    if (!Bluefruit.connected() && !Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.start(0);
  } else if (Bluefruit.Advertising.isRunning()) {
    Bluefruit.Advertising.stop();
    logf("Bluetooth advertising off");
  }
}

static void onBleConnect(uint16_t) { logf("Bluetooth: app connected"); }
static void onBleDisconnect(uint16_t, uint8_t) {
  bool keepOn = usbPowered() || (bleWindowOpen && !timeReached(bleUntilMs));
  if (keepOn) Bluefruit.Advertising.start(0);
}

static void onButton() { buttonFlag = true; }

static void bleBegin() {
  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName(devName);
  Bluefruit.Periph.setConnectCallback(onBleConnect);
  Bluefruit.Periph.setDisconnectCallback(onBleDisconnect);
  bleuart.begin();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(160, 800);   // 100 ms fast, 500 ms slow (units of 0.625 ms)
  Bluefruit.Advertising.setFastTimeout(30);
}

// ---------------- GPS ----------------
static void gpsUartOn() { Serial1.begin(gpsBaud ? gpsBaud : 115200); }

static void gpsUartOff() {
  Serial1.flush();
  Serial1.end();
  pinMode(PIN_SERIAL1_TX, OUTPUT);   // hold TX idle-high so the sleeping GPS is not woken by noise
  digitalWrite(PIN_SERIAL1_TX, HIGH);
}

static void gpsWakePulse() {
  for (int i = 0; i < 32; i++) Serial1.write(0xFF);
  Serial1.flush();
  delay(300);
  while (Serial1.available()) Serial1.read();
}

static void gpsSleep() {
  if (!gpsOk) return;
  gnss.powerOffWithInterrupt(0, VAL_RXM_PMREQ_WAKEUPSOURCE_UARTRX, true, 0);
  delay(100);
  gpsUartOff();
}

static bool gpsConfigure() {
  bool ok = gnss.setUART1Output(COM_TYPE_UBX);
  gnss.newCfgValset(VAL_LAYER_RAM_BBR);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_PVT_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_SAT_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_DOP_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_POSLLH_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_STATUS_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_VELNED_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_TIMEUTC_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_SIG_UART1, 0);
  gnss.addCfgValset8(UBLOX_CFG_MSGOUT_UBX_NAV_EOE_UART1, 0);
  gnss.sendCfgValset();
  ok &= gnss.setNavigationFrequency(1);
  gnss.setAutoPVT(false);
  gnss.setDynamicModel(DYN_MODEL_PEDESTRIAN);
  return ok;
}

static bool gpsBegin() {
  static const uint32_t bauds[] = {115200, 38400, 9600, 57600, 230400};
  gpsBaud = 0;
  for (uint32_t b : bauds) {
    wdtFeed();
    Serial1.begin(b);
    gpsWakePulse();
    if (gnss.begin(Serial1, 1200)) { gpsBaud = b; break; }
    Serial1.end();
  }
  if (!gpsBaud) {
    gpsOk = false;
    gpsUartOff();
    logf("GPS: no response at any baud rate. Check D6->GPS RX, D7<-GPS TX, power.");
    return false;
  }
  gpsOk = true;
  bool c = gpsConfigure();
  logf("GPS: found at %lu baud, configured %s", (unsigned long)gpsBaud, c ? "ok" : "with warnings");
  gpsSleep();
  return true;
}

static void serviceIO();  // forward

static bool gpsAcquire(uint32_t timeoutS, uint8_t &fixSecs, uint8_t &satsSeen) {
  uint32_t t0 = millis();
  fixSecs = 0; satsSeen = 0;
  if (!gpsOk && !gpsBegin()) return false;

  gpsUartOn();
  gpsWakePulse();
  if (!gnss.isConnected(1000)) {
    gpsWakePulse();
    if (!gnss.isConnected(1000)) {
      logf("GPS: did not wake, re-probing");
      Serial1.end();
      if (!gpsBegin()) return false;
      gpsUartOn();
      gpsWakePulse();
    }
  }

  led(LED_BLUE, true);
  bool got = false;
  Fix best = {false, 0, 0, 0, 0, 0};
  uint32_t firstValidAt = 0, lastSearchLog = 0;
  while (millis() - t0 < timeoutS * 1000UL) {
    wdtFeed();
    serviceIO();
    if (gnss.getPVT(1100)) {
      uint8_t ft = gnss.getFixType();
      bool fixOk = gnss.getGnssFixOk();
      uint8_t sats = gnss.getSIV();
      uint32_t haccMm = gnss.getHorizontalAccEst();
      if (sats > satsSeen) satsSeen = sats;
      if (fixOk && ft >= 2 && ft <= 4) {
        best.valid = true;
        best.lat = gnss.getLatitude();
        best.lon = gnss.getLongitude();
        best.hacc = haccMm / 1000 > 65535 ? 65535 : haccMm / 1000;
        best.sats = sats;
        best.atMs = millis();
        if (!firstValidAt) firstValidAt = millis();
        got = true;
        if (best.hacc <= cfg.good_hacc_m || millis() - firstValidAt > MAX_SETTLE_MS) break;
      } else if (millis() - lastSearchLog > 5000) {
        lastSearchLog = millis();
        logf("GPS: searching, fixType %u, %u sats", ft, sats);
      }
    }
    delay(800);
  }
  led(LED_BLUE, false);
  gpsSleep();

  uint32_t secs = (millis() - t0) / 1000;
  fixSecs = secs > 255 ? 255 : secs;
  if (got) {
    lastFix = best;
    firstFixDone = true;
    logf("GPS: fix %.6f, %.6f  +/-%um  %u sats  in %us", best.lat / 1e7, best.lon / 1e7, best.hacc, best.sats, fixSecs);
  } else {
    logf("GPS: no fix after %us (%u sats seen)", fixSecs, satsSeen);
  }
  return got;
}

// ---------------- radio ----------------
static void onRadioIrq() { rxFlag = true; }

static bool radioBegin() {
  radio.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, cfg.tx_dbm, PREAMBLE, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) { logf("Radio: init failed, code %d", st); return false; }
  radio.setDio2AsRfSwitch(true);
  radio.setCurrentLimit(140);
  radio.setPacketReceivedAction(onRadioIrq);
  radio.sleep();
  logf("Radio: ok, %.1f MHz SF%u BW%.0f, %d dBm", FREQ_MHZ, SF, BW_KHZ, cfg.tx_dbm);
  return true;
}

static const char *cmdName(uint8_t c) {
  switch (c) { case CMD_INTERVAL: return "interval"; case CMD_SLEEP: return "sleep"; case CMD_FIND: return "find";
               case CMD_POWER: return "power"; case CMD_NOW: return "now"; case CMD_GET_CONFIG: return "get_config";
               default: return "?"; }
}

// Applies an action/setting command. Returns ACK_APPLIED or ACK_REJECTED.
static uint8_t applyCommand(uint8_t code, uint32_t arg) {
  CollarConfig c = cfg;
  switch (code) {
    case CMD_INTERVAL:
      c.interval_s = arg; findActive = false;
      if (!applyConfig(c)) return ACK_REJECTED;
      nextReportMs = millis() + cfg.interval_s * 1000UL;
      break;
    case CMD_SLEEP:
      if (arg < 1 || arg > 24 * 60) return ACK_REJECTED;
      findActive = false;
      nextReportMs = millis() + arg * 60000UL;
      break;
    case CMD_FIND:
      if (arg < 1 || arg > 240) return ACK_REJECTED;
      findActive = true; findUntilMs = millis() + arg * 60000UL;
      nextReportMs = millis() + cfg.find_interval_s * 1000UL;
      break;
    case CMD_POWER:
      c.tx_dbm = (int8_t)(int32_t)arg;
      if ((int32_t)arg < -9 || (int32_t)arg > 22 || !applyConfig(c)) return ACK_REJECTED;
      break;
    case CMD_NOW:
      nextReportMs = millis();
      break;
    case CMD_GET_CONFIG:
      break;
    default:
      return ACK_REJECTED;
  }
  logf("Command: %s %ld", cmdName(code), (long)(int32_t)arg);
  return ACK_APPLIED;
}

// Handles one packet from the base. Returns true if it was a command for us (so we keep listening).
static bool handleBasePacket(const uint8_t *buf, size_t len) {
  if (!headerOk(buf, len)) return false;
  uint8_t type = buf[2];
  uint8_t cmdSeq = len >= 4 ? buf[3] : 0;
  bool dup = haveCmdSeq && cmdSeq == lastCmdSeq;

  if (type == PKT_CMD && len == sizeof(Cmd)) {
    Cmd c; memcpy(&c, buf, sizeof c);
    uint8_t status = dup ? ACK_DUPLICATE : applyCommand(c.code, c.arg);
    if (status == ACK_APPLIED) { lastCmdSeq = c.cmd_seq; haveCmdSeq = true; }
    delay(20);
    if (c.code == CMD_GET_CONFIG) {
      ConfigReply r = {MAGIC, NET_ID, PKT_CONFIG, c.cmd_seq, status, cfg};
      radio.transmit((uint8_t *)&r, sizeof r);
    } else {
      Ack a = {MAGIC, NET_ID, PKT_ACK, c.cmd_seq, c.code, status};
      radio.transmit((uint8_t *)&a, sizeof a);
    }
    return true;
  }
  if (type == PKT_SET_CONFIG && len == sizeof(SetConfig)) {
    SetConfig s; memcpy(&s, buf, sizeof s);
    uint8_t status = dup ? ACK_DUPLICATE : (applyConfig(s.cfg) ? ACK_APPLIED : ACK_REJECTED);
    if (status == ACK_APPLIED) { lastCmdSeq = s.cmd_seq; haveCmdSeq = true; }
    delay(20);
    ConfigReply r = {MAGIC, NET_ID, PKT_CONFIG, s.cmd_seq, status, cfg};
    radio.transmit((uint8_t *)&r, sizeof r);
    return true;
  }
  return false;  // chat traffic between bases etc.
}

// Listens for commands after a report; keeps listening while commands keep arriving.
static void listenForCommands() {
  uint32_t window = (uint32_t)cfg.rx_window_ds * 100;
  for (uint8_t n = 0; n < MAX_CMDS_PER_CHECKIN; n++) {
    rxFlag = false;
    if (radio.startReceive() != RADIOLIB_ERR_NONE) return;
    uint32_t t0 = millis();
    while (!rxFlag && millis() - t0 < window) delay(5);
    if (!rxFlag) { radio.standby(); return; }
    uint8_t buf[MAX_PACKET + 8];
    size_t len = radio.getPacketLength();
    if (len > sizeof buf) len = sizeof buf;
    int st = radio.readData(buf, len);
    radio.standby();
    if (st != RADIOLIB_ERR_NONE || !handleBasePacket(buf, len)) {
      // not for us: keep listening for the rest of this window
      window = MORE_CMD_WINDOW_MS;
      continue;
    }
    window = MORE_CMD_WINDOW_MS;
  }
}

// ---------------- report cycle ----------------
static void reportCycle() {
  if (findActive && timeReached(findUntilMs)) { findActive = false; logf("Find mode ended"); }

  uint32_t timeout = !firstFixDone ? cfg.first_fix_timeout_s : (findActive ? min<uint32_t>(cfg.fix_timeout_s, 25) : cfg.fix_timeout_s);
  uint8_t fixSecs = 0, satsSeen = 0;
  int8_t temp = readTempHalfC();   // before the GPS search and transmit warm the chip
  bool fresh = gpsAcquire(timeout, fixSecs, satsSeen);
  uint16_t mv = readBatteryMv();
  bool usb = usbPowered();

  ReportT r = {};
  r.magic = MAGIC; r.net = NET_ID; r.type = PKT_REPORT_T;
  r.temp_halfc = temp;
  r.seq = seq++;
  r.flags = (fresh ? F_FIX : 0) | (findActive ? F_FIND : 0) | (gpsOk ? 0 : F_GPS_FAIL) | (usb ? F_USB : 0) |
            (bootReport ? F_BOOT : 0) | ((!usb && mv < cfg.low_batt_mv) ? F_LOWBAT : 0) |
            ((Bluefruit.connected() || Bluefruit.Advertising.isRunning()) ? F_BLE : 0);
  r.lat_e7 = lastFix.valid ? lastFix.lat : 0;
  r.lon_e7 = lastFix.valid ? lastFix.lon : 0;
  r.hacc_m = lastFix.valid ? (uint8_t)min<uint16_t>(lastFix.hacc, HACC8_MAX) : HACC8_MAX;
  r.sats = fresh ? lastFix.sats : satsSeen;
  r.batt_mv = mv;
  r.fix_s = fixSecs;
  r.last_cmd = bootReport ? resetCode : lastCmdSeq;
  r.interval_s = (uint16_t)min<uint32_t>(currentIntervalS(), 65535);
  uint32_t ageMin = lastFix.valid ? (millis() - lastFix.atMs) / 60000UL : 255;
  r.age_min = ageMin > 255 ? 255 : ageMin;
  bootReport = false;

  nextReportMs = millis() + currentIntervalS() * 1000UL;  // a command below may change this

  if (!radioOk) radioOk = radioBegin();
  if (radioOk) {
    int st = radio.transmit((uint8_t *)&r, sizeof r);
    if (st != RADIOLIB_ERR_NONE) {
      // A supply dip (transmit current on a small battery) can reset the SX1262 on its own, losing the TCXO and
      // packet settings while the MCU keeps running; every later transmit then times out. Set it up again and retry.
      radioReinits++;
      logf("Radio: transmit failed, code %d; re-initialising (%u so far)", st, radioReinits);
      radioOk = radioBegin();
      if (radioOk) st = radio.transmit((uint8_t *)&r, sizeof r);
    }
    if (st == RADIOLIB_ERR_NONE) {
      logf("Sent report #%u (%s, batt %u mV, %.1f C)", r.seq, fresh ? "fresh fix" : "no fix", mv, temp / 2.0);
      blink(LED_GREEN, 40);
      listenForCommands();
    } else {
      logf("Radio: transmit failed, code %d", st);
      blink(LED_RED, 200);
    }
    radio.sleep();
  } else {
    blink(LED_RED, 200);
  }

  // Tell a connected app what was sent
  JsonDocument d;
  d["evt"] = "report"; d["seq"] = r.seq; d["fix"] = fresh; d["lat"] = r.lat_e7 / 1e7; d["lon"] = r.lon_e7 / 1e7;
  d["hacc_m"] = r.hacc_m; d["sats"] = r.sats; d["gps_s"] = r.fix_s; d["batt_mv"] = r.batt_mv;
  d["interval_s"] = r.interval_s; d["age_min"] = r.age_min; d["flags"] = r.flags; d["local"] = true;
  if (temp != TEMP_UNKNOWN) d["temp_c"] = temp / 2.0;
  sendJsonTo(d, false, true);
}

// ---------------- JSON + text command handling (USB and Bluetooth) ----------------
static void statusJson(JsonDocument &d) {
  d["evt"] = "status";
  d["dev"] = "collar"; d["fw"] = FW_VERSION; d["id"] = devId; d["name"] = devName;
  d["radio"] = radioOk; d["gps"] = gpsOk; d["gps_baud"] = gpsBaud;
  d["batt_mv"] = readBatteryMv(); d["usb"] = usbPowered();
  int8_t t = readTempHalfC(); if (t != TEMP_UNKNOWN) d["temp_c"] = t / 2.0;
  d["find"] = findActive; d["seq"] = seq;
  d["next_s"] = (int32_t)(nextReportMs - millis()) / 1000;
  d["uptime_s"] = millis() / 1000;
  d["reset"] = resetReasonName(resetCode); d["reset_code"] = resetCode; d["radio_reinits"] = radioReinits;
  if (lastFix.valid) {
    JsonObject f = d["last_fix"].to<JsonObject>();
    f["lat"] = lastFix.lat / 1e7; f["lon"] = lastFix.lon / 1e7; f["hacc_m"] = lastFix.hacc;
    f["sats"] = lastFix.sats; f["age_s"] = (millis() - lastFix.atMs) / 1000;
  }
  configToJson(cfg, d["config"].to<JsonObject>());
}

static void handleJson(const char *line, bool fromUsb, bool fromBle) {
  JsonDocument in;
  if (deserializeJson(in, line)) {
    JsonDocument e; e["evt"] = "error"; e["msg"] = "bad JSON"; sendJsonTo(e, fromUsb, fromBle); return;
  }
  const char *cmd = in["cmd"] | "";
  JsonDocument out;
  if (!strcmp(cmd, "hello") || !strcmp(cmd, "status")) {
    statusJson(out);
    if (!strcmp(cmd, "hello")) out["evt"] = "hello";
  } else if (!strcmp(cmd, "get_config")) {
    out["evt"] = "config"; configToJson(cfg, out["config"].to<JsonObject>());
  } else if (!strcmp(cmd, "set_config")) {
    CollarConfig c = cfg;
    configMergeJson(c, in["config"].as<JsonObjectConst>());
    if (applyConfig(c)) { out["evt"] = "config"; out["saved"] = true; configToJson(cfg, out["config"].to<JsonObject>()); }
    else { out["evt"] = "error"; out["msg"] = String("value out of range: ") + configRangeHelp(); }
  } else if (!strcmp(cmd, "reset_config")) {
    applyConfig(defaultConfig());
    out["evt"] = "config"; out["saved"] = true; configToJson(cfg, out["config"].to<JsonObject>());
  } else if (!strcmp(cmd, "dfu")) {
    // Restart into the UF2 bootloader so new firmware can be copied onto the XIAO drive (no button taps needed).
    out["evt"] = "dfu"; out["ok"] = true;
    sendJsonTo(out, fromUsb, fromBle);
    logf("Restarting into update mode");
    delay(300);
    enterUf2Dfu();
    return;
  } else if (!strcmp(cmd, "action")) {
    const char *a = in["action"] | "";
    long arg = in["arg"] | 0L;
    uint8_t code = !strcmp(a, "now") ? CMD_NOW : !strcmp(a, "find") ? CMD_FIND : !strcmp(a, "sleep") ? CMD_SLEEP : 0;
    uint8_t st = code ? applyCommand(code, (uint32_t)arg) : ACK_REJECTED;
    out["evt"] = "action"; out["action"] = a; out["ok"] = st == ACK_APPLIED;
  } else {
    out["evt"] = "error"; out["msg"] = "unknown cmd";
  }
  sendJsonTo(out, fromUsb, fromBle);
}

static void printStatusText() {
  logf("last reset: %s, up %lus, radio re-initialised %u times", resetReasonName(resetCode),
       (unsigned long)(millis() / 1000), radioReinits);
  logf("%s  interval %lus, find %s, tx %d dBm, radio %s, GPS %s @%lu, batt %u mV%s", FW_VERSION,
       (unsigned long)cfg.interval_s, findActive ? "ON" : "off", cfg.tx_dbm, radioOk ? "ok" : "FAIL",
       gpsOk ? "ok" : "FAIL", (unsigned long)gpsBaud, readBatteryMv(), usbPowered() ? " (USB)" : "");
  if (lastFix.valid)
    logf("last fix %.6f, %.6f +/-%um, %lu min ago", lastFix.lat / 1e7, lastFix.lon / 1e7, lastFix.hacc,
         (unsigned long)((millis() - lastFix.atMs) / 60000UL));
  logf("next report in %ld s", (long)((int32_t)(nextReportMs - millis()) / 1000));
}

static void handleTextLine(char *line) {
  char *cmd = strtok(line, " \t");
  char *argS = strtok(nullptr, " \t");
  if (!cmd) return;
  long arg = argS ? atol(argS) : 0;
  if      (!strcmp(cmd, "interval")) applyCommand(CMD_INTERVAL, (uint32_t)arg);
  else if (!strcmp(cmd, "sleep"))    applyCommand(CMD_SLEEP, (uint32_t)arg);
  else if (!strcmp(cmd, "find"))     applyCommand(CMD_FIND, (uint32_t)arg);
  else if (!strcmp(cmd, "power"))    applyCommand(CMD_POWER, (uint32_t)arg);
  else if (!strcmp(cmd, "now"))      applyCommand(CMD_NOW, 0);
  else if (!strcmp(cmd, "ble"))      bleStartWindow();
  else if (!strcmp(cmd, "status"))   printStatusText();
  else if (!strcmp(cmd, "dfu"))      { logf("Restarting into update mode"); delay(300); enterUf2Dfu(); }
  else logf("Commands: interval <sec> | sleep <min> | find <min> | power <dBm> | now | ble | dfu | status (or JSON lines)");
}

struct LineBuf { char buf[600]; uint16_t n; };
static LineBuf usbLine = {{0}, 0}, bleLine = {{0}, 0};

static void feedLine(LineBuf &lb, char ch, bool fromUsb, bool fromBle) {
  if (ch == '\r' || ch == '\n') {
    if (!lb.n) return;
    lb.buf[lb.n] = 0;
    if (lb.buf[0] == '{') handleJson(lb.buf, fromUsb, fromBle);
    else if (fromUsb) handleTextLine(lb.buf);
    lb.n = 0;
  } else if (lb.n < sizeof lb.buf - 1) {
    lb.buf[lb.n++] = ch;
  }
}

static void serviceIO() {
  while (Serial && Serial.available()) feedLine(usbLine, Serial.read(), true, false);
  while (bleuart.available()) feedLine(bleLine, bleuart.read(), false, true);
  bleService();
}

// ---------------- main ----------------
void setup() {
  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_BLUE, OUTPUT);
  ledsOff();
  pinMode(VBAT_ENABLE, OUTPUT); digitalWrite(VBAT_ENABLE, LOW);  // keep LOW: protects the battery-sense pin
  pinMode(PIN_USER_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_USER_BUTTON), onButton, FALLING);

  Serial.begin(115200);
  if (usbPowered()) { uint32_t t = millis(); while (!Serial && millis() - t < 3000) delay(10); }

  snprintf(devId, sizeof devId, "%08lx", (unsigned long)NRF_FICR->DEVICEID[0]);
  snprintf(devName, sizeof devName, "CatCollar-%.4s", devId + 4);
  resetCode = resetReasonCode(readResetReason());
  logf("%s starting, id %s, reset reason: %s (0x%02x)", FW_VERSION, devId, resetReasonName(resetCode), resetCode);

  InternalFS.begin();
  loadConfig();

#if USE_WATCHDOG
  wdtStart(180);
#endif

  bleBegin();
  bleStartWindow();
  radioOk = radioBegin();
  if (!radioOk) blink(LED_RED, 500);
  gpsBegin();
  nextReportMs = millis();
}

void loop() {
  wdtFeed();
  serviceIO();

  if (timeReached(nextReportMs)) { reportCycle(); return; }

  int32_t remaining = (int32_t)(nextReportMs - millis());
  if (remaining <= 0) return;
  // Short naps while a monitor or app is attached so commands feel instant; 1 s naps otherwise
  // (the CPU sleeps either way; 1 s keeps the button responsive).
  uint32_t chunk = (Serial || Bluefruit.connected()) ? 50 : 1000;
  delay(min<uint32_t>((uint32_t)remaining, chunk));
}
