// Cat tracker base station v2: Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262 + GC1109/KCT8103L front end + OLED)
//
// - Receives collar reports over LoRa, keeps the last 100 in memory, shows the latest on the OLED.
// - Talks to the web app over Bluetooth LE (Nordic UART service) and USB serial using JSON lines.
// - Queues commands and config for the collar and sends them in its listen window after each report.
// - Text chat with other bases on the same NET_ID (messages split into 14-byte fragments, acknowledged).
//
// See PROTOCOL.md in the repository for the JSON messages.
// Code lives here rather than in the .ino so the Arduino builder does not rewrite it.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <freertos/stream_buffer.h>
#include "protocol.h"
#include "config_json.h"

using namespace cat;

#define FW_VERSION "cat-base 2.3"

// ---------------- settings ----------------
static const int8_t   BASE_RADIO_DBM   = 10;     // SX1262 setting; the V4 front end adds ~10-13 dB
static const uint32_t ACK_WAIT_MS      = 1500;
static const uint32_t BUTTON_FIND_MIN  = 10;
static const size_t   HISTORY_LEN      = 100;
static const size_t   QUEUE_LEN        = 8;
static const size_t   MAX_NODES        = 16;
static const uint32_t HELLO_EVERY_MS   = 10UL * 60 * 1000;
static const uint32_t TEXT_ACK_WAIT_MS = 20000;

// ---------------- pins (Heltec V4) ----------------
static const uint8_t PIN_LORA_CS = 8, PIN_LORA_SCK = 9, PIN_LORA_MOSI = 10, PIN_LORA_MISO = 11;
static const uint8_t PIN_LORA_RST = 12, PIN_LORA_BUSY = 13, PIN_LORA_DIO1 = 14;
static const uint8_t PIN_FEM_POWER = 7, PIN_FEM_CSD = 2, PIN_GC1109_CPS = 46, PIN_KCT_CTX = 5;
static const uint8_t PIN_VEXT = 36, PIN_OLED_RST = 21, PIN_OLED_SCL = 18, PIN_OLED_SDA = 17;
static const uint8_t PIN_BUTTON = 0, PIN_LED = 35;

// Nordic UART service UUIDs (what the web app looks for)
#define NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // app -> base (write)
#define NUS_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // base -> app (notify)

SPIClass loraSpi(FSPI);
SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSpi);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
Preferences prefs;

enum FemType { FEM_NONE, FEM_GC1109, FEM_KCT8103L };
static FemType fem = FEM_NONE;
static volatile bool rxFlag = false;
static bool radioOk = false;
static uint32_t myId = 0;
static char myName[NODE_NAME_MAX + 1] = "Base";
static char bleName[20];

// ---------------- report history ----------------
struct HistEntry { Report r; float rssi, snr; uint32_t atMs; int8_t temp_halfc; };
static HistEntry hist[HISTORY_LEN];
static size_t histCount = 0, histHead = 0;   // head = next write slot
static bool haveReport = false;
static HistEntry lastRep;
static CollarConfig collarCfg; static bool collarCfgKnown = false; static uint32_t collarCfgAtMs = 0;

// ---------------- command queue ----------------
enum QKind : uint8_t { Q_CMD, Q_CONFIG };
struct QItem { bool used; uint16_t qid; QKind kind; uint8_t seq, code; uint32_t arg; CollarConfig cfg; uint8_t tries; };
static QItem queue[QUEUE_LEN];
static uint16_t nextQid = 1;
static uint8_t nextCmdSeq;
static bool awaitingReply = false;
static uint32_t replyDeadline = 0;
static char statusLine[32] = "";

// ---------------- chat ----------------
struct Node { uint32_t id; char name[NODE_NAME_MAX + 1]; uint32_t lastMs; float rssi, snr; };
static Node nodes[MAX_NODES];
static size_t nodeCount = 0;
struct Reasm { bool used; uint32_t from; uint16_t msgId; uint8_t total, gotMask; uint8_t len[TEXT_MAX_FRAGS];
               char data[TEXT_MAX_FRAGS][TEXT_FRAG_BYTES]; uint32_t startMs; float rssi, snr; };
static Reasm reasm[4];
struct Seen { uint32_t from; uint16_t msgId; };
static Seen seen[16]; static size_t seenHead = 0;
struct PendingAck { bool used; uint16_t msgId; uint32_t sentMs; };
static PendingAck pendingAcks[8];
static uint32_t nextHelloMs = 0;
static char lastChat[40] = ""; static uint32_t lastChatMs = 0;

// ---------------- BLE ----------------
static BLECharacteristic *bleTx = nullptr;
static BLEServer *bleServer = nullptr;
static StreamBufferHandle_t bleIn;
static volatile bool bleClient = false;
static volatile uint16_t bleMtu = 23;
static size_t histReplayLeft = 0;       // >0 while streaming history to the app
static bool nodesReplayPending = false;

// ================= output =================
static void bleSend(const char *data, size_t len) {
  if (!bleClient || !bleTx) return;
  size_t chunk = bleMtu > 23 ? bleMtu - 3 : 20;
  if (chunk > 244) chunk = 244;
  for (size_t off = 0; off < len; off += chunk) {
    size_t n = min(chunk, len - off);
    bleTx->setValue((uint8_t *)data + off, n);
    bleTx->notify();
    delay(4);
  }
}

static void emit(JsonDocument &doc, bool usb = true, bool ble = true) {
  char buf[640];
  size_t n = serializeJson(doc, buf, sizeof buf - 2);
  buf[n++] = '\n'; buf[n] = 0;
  if (usb) Serial.write((const uint8_t *)buf, n);
  if (ble) bleSend(buf, n);
}

static void logMsg(const char *fmt, ...) {
  char m[160];
  va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
  Serial.printf("# %s\n", m);
  JsonDocument d; d["evt"] = "log"; d["msg"] = m;
  emit(d, false, true);
}

static void idHex(uint32_t id, char *out) { snprintf(out, 9, "%08lx", (unsigned long)id); }

// ================= home, distance, screen helpers =================
// The base has no GPS. "Home" is its location, saved in flash (never in the source, the repo is public) and set from
// the collar's last fix (hold PRG, or {"cmd":"set_home"}) or from typed coordinates.
static bool homeSet = false;
static double homeLat = 0, homeLon = 0;

static void loadHome() {
  homeSet = prefs.getBool("home_set", false);
  homeLat = prefs.getDouble("home_lat", 0); homeLon = prefs.getDouble("home_lon", 0);
}
static void saveHome(double lat, double lon) {
  homeLat = lat; homeLon = lon; homeSet = true;
  prefs.putDouble("home_lat", lat); prefs.putDouble("home_lon", lon); prefs.putBool("home_set", true);
}
static void clearHome() { homeSet = false; prefs.putBool("home_set", false); }

static bool collarPosition(double &lat, double &lon) {
  if (!haveReport || (!lastRep.r.lat_e7 && !lastRep.r.lon_e7)) return false;
  lat = lastRep.r.lat_e7 / 1e7; lon = lastRep.r.lon_e7 / 1e7;
  return true;
}

// Great-circle distance (m) and initial bearing (degrees from north) from point 1 to point 2.
static void distBearing(double lat1, double lon1, double lat2, double lon2, double &m, double &deg) {
  const double R = 6371000.0, k = M_PI / 180;
  double p1 = lat1 * k, p2 = lat2 * k, dp = (lat2 - lat1) * k, dl = (lon2 - lon1) * k;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  m = 2 * R * atan2(sqrt(a), sqrt(1 - a));
  deg = fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / k + 360, 360);
}
static const char *compass8(double deg) {
  static const char *n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return n[(int)((deg + 22.5) / 45) % 8];
}

// Link quality 0-100 at SF10: 0 near the decoding limit (SNR -15 dB or RSSI -125 dBm), 100 at SNR +10 dB and
// RSSI -60 dBm or better; the weaker of the two sets the figure. Same formula as signalPercent() in the app.
static int signalPct(float rssi, float snr) {
  float q = min((rssi + 125) / 65.0f, (snr + 15) / 25.0f);
  return (int)constrain(lroundf(q * 100), 0L, 100L);
}

// Same LiPo curve as battPercent() in the app (0% at the 3.0 V cutoff).
static int battPct(uint16_t mv) {
  static const uint16_t v[] = {3000, 3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 3950, 4000, 4100, 4200};
  static const uint8_t  p[] = {0, 2, 5, 12, 30, 42, 55, 65, 74, 82, 88, 96, 100};
  if (mv <= v[0]) return 0;
  for (int i = 1; i < 13; i++)
    if (mv <= v[i]) return p[i - 1] + (int)((long)(mv - v[i - 1]) * (p[i] - p[i - 1]) / (v[i] - v[i - 1]));
  return 100;
}

// Short message shown as a banner on the screen for a few seconds.
static char noticeMsg[24] = ""; static uint32_t noticeUntil = 0;
static void notice(const char *m) { snprintf(noticeMsg, sizeof noticeMsg, "%s", m); noticeUntil = millis() + 3000; }

// ================= front end + radio =================
static void femInit() {
  pinMode(PIN_FEM_POWER, OUTPUT); digitalWrite(PIN_FEM_POWER, HIGH);
  delay(2);
  pinMode(PIN_FEM_CSD, INPUT);
  delay(2);
  if (digitalRead(PIN_FEM_CSD) == HIGH) {
    fem = FEM_KCT8103L;
    pinMode(PIN_FEM_CSD, OUTPUT); digitalWrite(PIN_FEM_CSD, HIGH);
    pinMode(PIN_KCT_CTX, OUTPUT); digitalWrite(PIN_KCT_CTX, LOW);
  } else {
    fem = FEM_GC1109;
    pinMode(PIN_FEM_CSD, OUTPUT); digitalWrite(PIN_FEM_CSD, HIGH);
    pinMode(PIN_GC1109_CPS, OUTPUT); digitalWrite(PIN_GC1109_CPS, LOW);
  }
}
static void femTx(bool tx) {
  if (fem == FEM_GC1109) digitalWrite(PIN_GC1109_CPS, tx ? HIGH : LOW);
  else if (fem == FEM_KCT8103L) digitalWrite(PIN_KCT_CTX, tx ? HIGH : LOW);
}

static void onRadioIrq() { rxFlag = true; }
static void startRx() { femTx(false); rxFlag = false; radio.startReceive(); }

static bool radioBegin() {
  loraSpi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  femInit();
  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, BASE_RADIO_DBM, PREAMBLE, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) { logMsg("Radio init failed, code %d", st); return false; }
  radio.setDio2AsRfSwitch(true);
  radio.setCurrentLimit(140);
  radio.setRxBoostedGainMode(true);
  radio.setPacketReceivedAction(onRadioIrq);
  logMsg("Radio ok: %.1f MHz SF%u BW%.0f, front end %s", FREQ_MHZ, SF, BW_KHZ,
         fem == FEM_GC1109 ? "GC1109" : fem == FEM_KCT8103L ? "KCT8103L" : "none");
  startRx();
  return true;
}

// Transmit immediately (used inside the collar's listen window).
static void sendNow(const void *data, size_t len) {
  femTx(true);
  int st = radio.transmit((const uint8_t *)data, len);
  femTx(false);
  if (st != RADIOLIB_ERR_NONE) logMsg("Transmit failed, code %d", st);
}

// Listen before talk, then transmit (chat and hellos).
static void sendPolite(const void *data, size_t len) {
  for (int i = 0; i < 6; i++) {
    int s = radio.scanChannel();
    if (s == RADIOLIB_CHANNEL_FREE) break;
    delay(random(150, 450));
  }
  sendNow(data, len);
}

// ================= nodes =================
static Node *findNode(uint32_t id) { for (size_t i = 0; i < nodeCount; i++) if (nodes[i].id == id) return &nodes[i]; return nullptr; }

static Node *touchNode(uint32_t id, float rssi, float snr) {
  Node *n = findNode(id);
  if (!n) {
    if (nodeCount < MAX_NODES) n = &nodes[nodeCount++];
    else { n = &nodes[0]; for (size_t i = 1; i < nodeCount; i++) if (nodes[i].lastMs < n->lastMs) n = &nodes[i]; }
    n->id = id; snprintf(n->name, sizeof n->name, "%04lx", (unsigned long)(id & 0xFFFF));
  }
  n->lastMs = millis(); n->rssi = rssi; n->snr = snr;
  return n;
}

static void emitNode(const Node &n, bool usb = true) {
  JsonDocument d; char id[9]; idHex(n.id, id);
  d["evt"] = "node"; d["id"] = id; d["name"] = n.name; d["rssi"] = n.rssi; d["snr"] = n.snr;
  d["ago_ms"] = millis() - n.lastMs;
  emit(d, usb, true);
}

static void sendHello() {
  Hello h = {}; h.magic = MAGIC; h.net = NET_ID; h.type = PKT_HELLO; h.id = myId;
  size_t nl = strnlen(myName, NODE_NAME_MAX);
  memcpy(h.name, myName, nl);
  sendPolite(&h, HELLO_HEADER + nl);
  startRx();
}

// ================= reports + collar replies =================
static void reportJson(JsonDocument &d, const HistEntry &e, bool hist) {
  const Report &r = e.r;
  d["evt"] = "report"; d["seq"] = r.seq; d["fix"] = (r.flags & F_FIX) != 0;
  if (r.lat_e7 || r.lon_e7) { d["lat"] = r.lat_e7 / 1e7; d["lon"] = r.lon_e7 / 1e7; }
  d["hacc_m"] = r.hacc_m; d["sats"] = r.sats; d["gps_s"] = r.fix_s; d["batt_mv"] = r.batt_mv;
  d["interval_s"] = r.interval_s; d["age_min"] = r.age_min; d["flags"] = r.flags;
  d["rssi"] = e.rssi; d["snr"] = e.snr; d["ago_ms"] = millis() - e.atMs;
  if (r.flags & F_BOOT) d["reset"] = resetReasonName(r.last_cmd);
  if (e.temp_halfc != TEMP_UNKNOWN) d["temp_c"] = e.temp_halfc / 2.0;
  if (hist) d["hist"] = true;
}

static void emitQueue();
static void sendHeadOfQueue();

static QItem *queueHead() {
  for (size_t i = 0; i < QUEUE_LEN; i++) if (queue[i].used) return &queue[i];  // queue is kept in order
  return nullptr;
}

static void queueRemove(QItem *it) {
  it->used = false;
  // compact so the first used slot stays the oldest
  QItem tmp[QUEUE_LEN]; size_t k = 0;
  for (size_t i = 0; i < QUEUE_LEN; i++) if (queue[i].used) tmp[k++] = queue[i];
  for (size_t i = 0; i < QUEUE_LEN; i++) queue[i] = i < k ? tmp[i] : QItem{};
}

static const char *cmdName(uint8_t c) {
  switch (c) { case CMD_INTERVAL: return "interval"; case CMD_SLEEP: return "sleep"; case CMD_FIND: return "find";
               case CMD_POWER: return "power"; case CMD_NOW: return "now"; case CMD_GET_CONFIG: return "get_config";
               default: return "?"; }
}

static void emitCmdState(const QItem &it, const char *state) {
  JsonDocument d;
  d["evt"] = "cmd"; d["qid"] = it.qid; d["state"] = state;
  d["action"] = it.kind == Q_CONFIG ? "config" : cmdName(it.code);
  if (it.kind == Q_CMD) d["arg"] = (int32_t)it.arg;
  d["tries"] = it.tries;
  emit(d);
  snprintf(statusLine, sizeof statusLine, "%s %s", it.kind == Q_CONFIG ? "config" : cmdName(it.code), state);
  if (!strcmp(state, "done") || !strcmp(state, "rejected")) notice(statusLine);
}

static void finishHead(const char *state) {
  QItem *h = queueHead();
  if (!h) return;
  emitCmdState(*h, state);
  queueRemove(h);
  awaitingReply = false;
  emitQueue();
}

static void onReport(const Report &r, float rssi, float snr, int8_t tempHalfC) {
  HistEntry e = {r, rssi, snr, millis(), tempHalfC};
  hist[histHead] = e; histHead = (histHead + 1) % HISTORY_LEN; if (histCount < HISTORY_LEN) histCount++;
  lastRep = e; haveReport = true;

  digitalWrite(PIN_LED, HIGH);
  Serial.printf("# collar #%u %s %.6f,%.6f +/-%um sats %u batt %.2fV rssi %.0f snr %.1f\n", r.seq,
                (r.flags & F_FIX) ? "FIX" : "old", r.lat_e7 / 1e7, r.lon_e7 / 1e7, r.hacc_m, r.sats,
                r.batt_mv / 1000.0, rssi, snr);
  if (tempHalfC != TEMP_UNKNOWN) Serial.printf("# collar temperature %.1f C\n", tempHalfC / 2.0);
  if (r.flags & F_BOOT) Serial.printf("# collar restarted: %s\n", resetReasonName(r.last_cmd));
  JsonDocument d; reportJson(d, e, false); emit(d);
  digitalWrite(PIN_LED, LOW);

  QItem *h = queueHead();
  // applied earlier, our reply was lost (a boot report carries the reset reason in last_cmd instead)
  if (h && h->tries > 0 && !(r.flags & F_BOOT) && r.last_cmd == h->seq) finishHead("done");
  if (queueHead()) sendHeadOfQueue();
}

static void onCollarReply(uint8_t seq, uint8_t status, const CollarConfig *cfg) {
  if (cfg) {
    collarCfg = *cfg; collarCfgKnown = true; collarCfgAtMs = millis();
    JsonDocument d; d["evt"] = "collar_config"; configToJson(collarCfg, d["config"].to<JsonObject>()); d["ago_ms"] = 0;
    emit(d);
  }
  QItem *h = queueHead();
  if (!h || h->seq != seq) return;
  finishHead(status == ACK_REJECTED ? "rejected" : "done");
  if (queueHead()) sendHeadOfQueue();   // collar keeps listening briefly for more
}

// ================= command queue =================
static void emitQueue() {
  JsonDocument d; d["evt"] = "queue";
  JsonArray a = d["items"].to<JsonArray>();
  for (size_t i = 0; i < QUEUE_LEN; i++) if (queue[i].used) {
    JsonObject o = a.add<JsonObject>();
    o["qid"] = queue[i].qid; o["action"] = queue[i].kind == Q_CONFIG ? "config" : cmdName(queue[i].code);
    if (queue[i].kind == Q_CMD) o["arg"] = (int32_t)queue[i].arg;
    o["tries"] = queue[i].tries;
  }
  emit(d);
}

static int queueAdd(QKind kind, uint8_t code, uint32_t arg, const CollarConfig *cfg) {
  // a newer config replaces an older queued one
  if (kind == Q_CONFIG) for (size_t i = 0; i < QUEUE_LEN; i++)
    if (queue[i].used && queue[i].kind == Q_CONFIG && queue[i].tries == 0) { queue[i].cfg = *cfg; emitQueue(); return queue[i].qid; }
  for (size_t i = 0; i < QUEUE_LEN; i++) if (!queue[i].used) {
    QItem &it = queue[i];
    it = QItem{}; it.used = true; it.qid = nextQid++; it.kind = kind; it.seq = nextCmdSeq++;
    if (nextCmdSeq == 0) nextCmdSeq = 1;
    it.code = code; it.arg = arg; if (cfg) it.cfg = *cfg;
    emitCmdState(it, "queued");
    emitQueue();
    return it.qid;
  }
  return -1;
}

static void sendHeadOfQueue() {
  QItem *h = queueHead();
  if (!h) return;
  delay(40);   // collar switches to receive right after it transmits
  if (h->kind == Q_CMD) { Cmd c = {MAGIC, NET_ID, PKT_CMD, h->seq, h->code, h->arg}; sendNow(&c, sizeof c); }
  else { SetConfig s = {MAGIC, NET_ID, PKT_SET_CONFIG, h->seq, h->cfg}; sendNow(&s, sizeof s); }
  h->tries++;
  awaitingReply = true;
  replyDeadline = millis() + ACK_WAIT_MS;
  emitCmdState(*h, "sent");
  startRx();
}

// ================= chat =================
static bool alreadySeen(uint32_t from, uint16_t msgId) {
  for (auto &s : seen) if (s.from == from && s.msgId == msgId) return true;
  return false;
}

static void onTextFrag(const uint8_t *buf, size_t len, float rssi, float snr) {
  if (len <= TEXT_HEADER) return;
  TextFrag f; memset(&f, 0, sizeof f); memcpy(&f, buf, min(len, sizeof f));
  if (f.from == myId) return;
  uint8_t idx = f.frag >> 4, total = f.frag & 0x0F;
  if (total == 0 || total > TEXT_MAX_FRAGS || idx >= total) return;
  touchNode(f.from, rssi, snr);

  Reasm *r = nullptr;
  for (auto &x : reasm) if (x.used && x.from == f.from && x.msgId == f.msg_id) r = &x;
  if (!r) {
    for (auto &x : reasm) if (!x.used || millis() - x.startMs > 60000) { r = &x; break; }
    if (!r) r = &reasm[0];
    memset(r, 0, sizeof *r);
    r->used = true; r->from = f.from; r->msgId = f.msg_id; r->total = total; r->startMs = millis();
  }
  r->len[idx] = len - TEXT_HEADER;
  memcpy(r->data[idx], f.text, r->len[idx]);
  r->gotMask |= 1 << idx; r->rssi = rssi; r->snr = snr;
  if (r->gotMask != (1 << r->total) - 1) return;

  // complete
  char text[TEXT_MAX_BYTES + 1]; size_t n = 0;
  for (uint8_t i = 0; i < r->total; i++) { memcpy(text + n, r->data[i], r->len[i]); n += r->len[i]; }
  text[n] = 0;
  bool dup = alreadySeen(r->from, r->msgId);
  r->used = false;

  TextAck a = {MAGIC, NET_ID, PKT_TEXT_ACK, myId, f.from, f.msg_id};
  delay(random(100, 600));
  sendPolite(&a, sizeof a);
  startRx();
  if (dup) return;
  seen[seenHead] = {f.from, f.msg_id}; seenHead = (seenHead + 1) % 16;

  Node *nd = findNode(f.from);
  JsonDocument d; char id[9]; idHex(f.from, id);
  d["evt"] = "text"; d["from"] = id; d["name"] = nd ? nd->name : id; d["msg_id"] = f.msg_id; d["text"] = text;
  d["rssi"] = rssi; d["snr"] = snr;
  emit(d);
  snprintf(lastChat, sizeof lastChat, "%.8s: %s", nd ? nd->name : id, text);
  lastChatMs = millis();
  char m[24]; snprintf(m, sizeof m, "Msg from %.12s", nd ? nd->name : id); notice(m);
}

static void sendText(const char *text, const char *cid) {
  size_t len = strnlen(text, TEXT_MAX_BYTES);
  if (len == 0) { JsonDocument e; e["evt"] = "error"; e["msg"] = "empty message"; emit(e); return; }
  uint8_t total = len == 0 ? 1 : (len + TEXT_FRAG_BYTES - 1) / TEXT_FRAG_BYTES;
  uint16_t msgId = (uint16_t)esp_random();
  JsonDocument d; d["evt"] = "text_sent"; d["msg_id"] = msgId; d["frags"] = total; d["cid"] = cid;
  if (strlen(text) > TEXT_MAX_BYTES) d["truncated"] = true;
  emit(d);
  for (uint8_t i = 0; i < total; i++) {
    TextFrag f = {}; f.magic = MAGIC; f.net = NET_ID; f.type = PKT_TEXT; f.from = myId; f.msg_id = msgId;
    f.frag = (i << 4) | total;
    size_t n = min(TEXT_FRAG_BYTES, len - i * TEXT_FRAG_BYTES);
    memcpy(f.text, text + i * TEXT_FRAG_BYTES, n);
    sendPolite(&f, TEXT_HEADER + n);
    delay(60);
  }
  for (auto &p : pendingAcks) if (!p.used) { p = {true, msgId, millis()}; break; }
  startRx();
}

static void onTextAck(const uint8_t *buf, size_t len, float rssi, float snr) {
  if (len != sizeof(TextAck)) return;
  TextAck a; memcpy(&a, buf, sizeof a);
  touchNode(a.from, rssi, snr);
  if (a.to != myId) return;
  Node *nd = findNode(a.from);
  JsonDocument d; char id[9]; idHex(a.from, id);
  d["evt"] = "text_ack"; d["msg_id"] = a.msg_id; d["by"] = id; d["by_name"] = nd ? nd->name : id; d["rssi"] = rssi;
  emit(d);
  for (auto &p : pendingAcks) if (p.used && p.msgId == a.msg_id) p.used = false;
}

static void onHello(const uint8_t *buf, size_t len, float rssi, float snr) {
  if (len < HELLO_HEADER + 1 || len > sizeof(Hello)) return;
  Hello h; memset(&h, 0, sizeof h); memcpy(&h, buf, len);
  if (h.id == myId) return;
  Node *n = touchNode(h.id, rssi, snr);
  size_t nl = len - HELLO_HEADER;
  memcpy(n->name, h.name, nl); n->name[nl] = 0;
  emitNode(*n);
}

// ================= packet dispatch =================
static void handlePacket() {
  uint8_t buf[MAX_PACKET + 8];
  size_t len = radio.getPacketLength();
  if (len > sizeof buf) len = sizeof buf;
  int st = radio.readData(buf, len);
  float rssi = radio.getRSSI(), snr = radio.getSNR();
  startRx();
  if (st != RADIOLIB_ERR_NONE || !headerOk(buf, len)) return;

  switch (buf[2]) {
    case PKT_REPORT:
      if (len == sizeof(Report)) { Report r; memcpy(&r, buf, sizeof r); onReport(r, rssi, snr, TEMP_UNKNOWN); }
      break;
    case PKT_REPORT_T:
      if (len == sizeof(ReportT)) {
        ReportT t; memcpy(&t, buf, sizeof t);
        Report r = {t.magic, t.net, PKT_REPORT, t.seq, t.flags, t.lat_e7, t.lon_e7, t.hacc_m, t.sats, t.batt_mv,
                    t.fix_s, t.last_cmd, t.interval_s, t.age_min};
        onReport(r, rssi, snr, t.temp_halfc);
      }
      break;
    case PKT_ACK:
      if (len == sizeof(Ack)) { Ack a; memcpy(&a, buf, sizeof a); onCollarReply(a.cmd_seq, a.status, nullptr); }
      break;
    case PKT_CONFIG:
      if (len == sizeof(ConfigReply)) { ConfigReply c; memcpy(&c, buf, sizeof c); onCollarReply(c.cmd_seq, c.status, &c.cfg); }
      break;
    case PKT_TEXT:     onTextFrag(buf, len, rssi, snr); break;
    case PKT_TEXT_ACK: onTextAck(buf, len, rssi, snr); break;
    case PKT_HELLO:    onHello(buf, len, rssi, snr); break;
    default: break;
  }
}

// ================= app commands =================
static void helloJson(JsonDocument &d) {
  char id[9]; idHex(myId, id);
  d["evt"] = "hello"; d["dev"] = "base"; d["fw"] = FW_VERSION; d["id"] = id; d["name"] = myName;
  d["uptime_ms"] = millis(); d["radio"] = radioOk; d["freq_mhz"] = FREQ_MHZ; d["sf"] = SF; d["bw_khz"] = BW_KHZ;
  d["net_id"] = NET_ID; d["history"] = histCount; d["text_max"] = TEXT_MAX_BYTES;
  if (collarCfgKnown) { configToJson(collarCfg, d["collar_config"].to<JsonObject>()); d["collar_config_ago_ms"] = millis() - collarCfgAtMs; }
  if (homeSet) { JsonObject h = d["home"].to<JsonObject>(); h["lat"] = homeLat; h["lon"] = homeLon; }
}

static void emitHome(bool usb, bool ble) {
  JsonDocument d; d["evt"] = "home"; d["set"] = homeSet;
  if (homeSet) { d["lat"] = homeLat; d["lon"] = homeLon; }
  emit(d, usb, ble);
}

// Sets home from the collar's last reported position. Returns false if there is none yet.
static bool setHomeFromCollar() {
  double lat, lon;
  if (!collarPosition(lat, lon)) { notice("No collar position"); return false; }
  saveHome(lat, lon);
  logMsg("Home set from collar #%u: %.6f, %.6f", lastRep.r.seq, lat, lon);
  notice("Home set");
  return true;
}

static uint8_t actionCode(const char *a) {
  if (!strcmp(a, "interval")) return CMD_INTERVAL;
  if (!strcmp(a, "sleep")) return CMD_SLEEP;
  if (!strcmp(a, "find")) return CMD_FIND;
  if (!strcmp(a, "power")) return CMD_POWER;
  if (!strcmp(a, "now")) return CMD_NOW;
  if (!strcmp(a, "get_config")) return CMD_GET_CONFIG;
  return 0;
}

static void handleJson(const char *line, bool fromUsb) {
  JsonDocument in;
  JsonDocument out;
  if (deserializeJson(in, line)) { out["evt"] = "error"; out["msg"] = "bad JSON"; emit(out, fromUsb, !fromUsb); return; }
  const char *cmd = in["cmd"] | "";

  if (!strcmp(cmd, "hello")) { helloJson(out); emit(out, fromUsb, !fromUsb); emitQueue(); return; }
  if (!strcmp(cmd, "history")) {
    if (fromUsb) {
      for (size_t i = 0; i < histCount; i++) {
        const HistEntry &e = hist[(histHead + HISTORY_LEN - histCount + i) % HISTORY_LEN];
        JsonDocument d; reportJson(d, e, true); emit(d, true, false);
      }
      for (size_t i = 0; i < nodeCount; i++) emitNode(nodes[i]);
      JsonDocument d; d["evt"] = "history_end"; emit(d, true, false);
    } else {
      histReplayLeft = histCount; nodesReplayPending = true;   // streamed from loop() so radio stays responsive
    }
    return;
  }
  if (!strcmp(cmd, "collar")) {
    const char *a = in["action"] | "";
    uint8_t code = actionCode(a);
    if (!code) { out["evt"] = "error"; out["msg"] = "unknown action"; emit(out, fromUsb, !fromUsb); return; }
    int q = queueAdd(Q_CMD, code, (uint32_t)(int32_t)(in["arg"] | 0L), nullptr);
    if (q < 0) { out["evt"] = "error"; out["msg"] = "queue full"; emit(out, fromUsb, !fromUsb); }
    return;
  }
  if (!strcmp(cmd, "collar_config")) {
    CollarConfig c = collarCfgKnown ? collarCfg : defaultConfig();
    configMergeJson(c, in["config"].as<JsonObjectConst>());
    if (!configValid(c)) { out["evt"] = "error"; out["msg"] = String("out of range: ") + configRangeHelp(); emit(out, fromUsb, !fromUsb); return; }
    if (queueAdd(Q_CONFIG, 0, 0, &c) < 0) { out["evt"] = "error"; out["msg"] = "queue full"; emit(out, fromUsb, !fromUsb); }
    return;
  }
  if (!strcmp(cmd, "cancel")) {
    long qid = in["qid"] | 0L;
    for (size_t i = 0; i < QUEUE_LEN; i++)
      if (queue[i].used && (qid == 0 || queue[i].qid == qid)) { emitCmdState(queue[i], "cancelled"); queue[i].used = false; }
    queueRemove(&queue[0]);  // compacts (queue[0] may already be unused)
    awaitingReply = false;
    emitQueue();
    return;
  }
  if (!strcmp(cmd, "queue")) { emitQueue(); return; }
  if (!strcmp(cmd, "send_text")) { sendText(in["text"] | "", in["cid"] | ""); return; }
  if (!strcmp(cmd, "set_name")) {
    const char *n = in["name"] | "";
    if (!*n) { out["evt"] = "error"; out["msg"] = "empty name"; emit(out, fromUsb, !fromUsb); return; }
    snprintf(myName, sizeof myName, "%s", n);
    prefs.putString("name", myName);
    helloJson(out); emit(out, fromUsb, !fromUsb);
    sendHello();
    return;
  }
  if (!strcmp(cmd, "nodes")) { for (size_t i = 0; i < nodeCount; i++) emitNode(nodes[i], fromUsb); return; }
  if (!strcmp(cmd, "set_home")) {
    if (in["clear"] | false) { clearHome(); notice("Home cleared"); }
    else if (in["lat"].is<double>() && in["lon"].is<double>()) {
      double lat = in["lat"], lon = in["lon"];
      if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { out["evt"] = "error"; out["msg"] = "bad coordinates"; emit(out, fromUsb, !fromUsb); return; }
      saveHome(lat, lon); notice("Home set");
    } else if (!setHomeFromCollar()) {
      out["evt"] = "error"; out["msg"] = "no collar position yet"; emit(out, fromUsb, !fromUsb); return;
    }
    emitHome(true, true);
    return;
  }
  out["evt"] = "error"; out["msg"] = "unknown cmd"; emit(out, fromUsb, !fromUsb);
}

// USB also accepts the simple text commands from v1
static void handleTextLine(char *line) {
  char *cmd = strtok(line, " \t");
  char *argS = strtok(nullptr, " \t");
  if (!cmd) return;
  long arg = argS ? atol(argS) : 0;
  uint8_t code = actionCode(cmd);
  if (code) { if (code == CMD_FIND && !arg) arg = BUTTON_FIND_MIN; queueAdd(Q_CMD, code, (uint32_t)arg, nullptr); }
  else if (!strcmp(cmd, "cancel")) { handleJson("{\"cmd\":\"cancel\"}", true); }
  else if (!strcmp(cmd, "home")) {
    char *lonS = argS ? strtok(nullptr, " \t") : nullptr;
    if (argS && lonS) saveHome(atof(argS), atof(lonS)); else setHomeFromCollar();
    emitHome(true, false);
  }
  else if (!strcmp(cmd, "say")) { char *rest = argS ? argS + strlen(argS) + 1 : nullptr; String t = argS ? String(argS) : "";
                                  if (rest && *rest) { t += " "; t += strtok(rest, "\r\n"); } sendText(t.c_str(), ""); }
  else Serial.println("# Commands: interval <s> | find <min> | sleep <min> | power <dBm> | now | get_config | cancel | say <text> | home [<lat> <lon>] | or JSON lines");
}

struct LineBuf { char buf[700]; uint16_t n; };
static LineBuf usbLine = {{0}, 0}, bleLine = {{0}, 0};

static void feedLine(LineBuf &lb, char ch, bool fromUsb) {
  if (ch == '\r' || ch == '\n') {
    if (!lb.n) return;
    lb.buf[lb.n] = 0;
    if (lb.buf[0] == '{') handleJson(lb.buf, fromUsb);
    else if (fromUsb) handleTextLine(lb.buf);
    lb.n = 0;
  } else if (lb.n < sizeof lb.buf - 1) lb.buf[lb.n++] = ch;
}

// ================= BLE =================
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleClient = true; bleMtu = 23; }
  void onDisconnect(BLEServer *) override { bleClient = false; histReplayLeft = 0; BLEDevice::startAdvertising(); }
  void onMtuChanged(BLEServer *, ble_gap_conn_desc *, uint16_t mtu) override { bleMtu = mtu; }
};
class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length()) xStreamBufferSend(bleIn, v.c_str(), v.length(), 0);
  }
};

static void bleBegin() {
  bleIn = xStreamBufferCreate(2048, 1);
  BLEDevice::init(bleName);
  BLEDevice::setMTU(247);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCb());
  BLEService *svc = bleServer->createService(NUS_SERVICE);
  bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());
  BLECharacteristic *rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ================= display =================
// Layout (128x64): compass on the left (needle points from the base toward the collar), and on the right the distance,
// direction and age of the position, then battery, signal and temperature. Banners for a few seconds on events.
static const int COMP_X = 31, COMP_Y = 33, COMP_R = 29;   // compass centre and radius
static const int COL_X = 67;                     // right column x

static void drawCentered(int y, const char *s) { oled.drawUTF8((128 - oled.getUTF8Width(s)) / 2, y, s); }

static void fmtAge(uint32_t s, char *out, size_t n) {
  if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else if (s < 172800) snprintf(out, n, "%luh", (unsigned long)(s / 3600));
  else snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}

// One tiny satellite (two solar panels and a body, 9x3 px) centred on (x, y), with a gap cut in whatever is behind it.
static void drawSatellite(int x, int y) {
  oled.setDrawColor(0); oled.drawBox(x - 5, y - 2, 11, 5); oled.setDrawColor(1);
  oled.drawBox(x - 4, y - 1, 2, 3);   // left panel
  oled.drawPixel(x - 2, y);           // arm
  oled.drawFrame(x - 1, y - 1, 3, 3); // body
  oled.drawPixel(x + 2, y);           // arm
  oled.drawBox(x + 3, y - 1, 2, 3);   // right panel
}

// One satellite per satellite in use (capped at 12), spread evenly around the ring like satellites in the sky.
// Offset by half a step so none sits on the "N".
static void drawSats(uint8_t sats) {
  int n = min<int>(sats, 12);
  for (int i = 0; i < n; i++) {
    double a = (i + 0.5) * 2 * M_PI / n;
    drawSatellite(lround(COMP_X + sin(a) * (COMP_R - 2)), lround(COMP_Y - cos(a) * (COMP_R - 2)));
  }
}

static void drawCompass(bool haveDir, double bearing, bool atHome, uint8_t sats) {
  oled.drawCircle(COMP_X, COMP_Y, COMP_R);
  oled.drawLine(COMP_X + COMP_R - 3, COMP_Y, COMP_X + COMP_R, COMP_Y);   // E, S, W ticks
  oled.drawLine(COMP_X, COMP_Y + COMP_R - 3, COMP_X, COMP_Y + COMP_R);
  oled.drawLine(COMP_X - COMP_R, COMP_Y, COMP_X - COMP_R + 3, COMP_Y);
  drawSats(sats);
  oled.setDrawColor(0); oled.drawBox(COMP_X - 4, 0, 9, 9); oled.setDrawColor(1);   // gap in the ring for "N"
  oled.setFont(u8g2_font_helvB08_tf); oled.drawStr(COMP_X - oled.getStrWidth("N") / 2 + 1, 8, "N");

  if (atHome) { oled.drawDisc(COMP_X, COMP_Y, 5); oled.drawCircle(COMP_X, COMP_Y, 9); return; }
  if (!haveDir) { oled.setFont(u8g2_font_helvB14_tf); oled.drawStr(COMP_X - oled.getStrWidth("?") / 2 + 1, COMP_Y + 7, "?"); return; }

  double a = bearing * M_PI / 180, dx = sin(a), dy = -cos(a), px = -dy, py = dx;
  int tipX = lround(COMP_X + dx * (COMP_R - 8)), tipY = lround(COMP_Y + dy * (COMP_R - 8));   // clear of the satellites
  double hb = COMP_R - 17;   // where the head starts
  int hx = lround(COMP_X + dx * hb), hy = lround(COMP_Y + dy * hb);
  for (int o = -1; o <= 1; o++)   // 3 px shaft
    oled.drawLine(lround(COMP_X + px * o), lround(COMP_Y + py * o), lround(hx + px * o), lround(hy + py * o));
  oled.drawTriangle(tipX, tipY, lround(hx + px * 6), lround(hy + py * 6), lround(hx - px * 6), lround(hy - py * 6));
  oled.drawDisc(COMP_X, COMP_Y, 3);       // the base station
}

static void drawBattery(int x, int y, int pct, bool usb) {   // y = text baseline
  oled.drawFrame(x, y - 7, 14, 7); oled.drawBox(x + 14, y - 5, 2, 3);
  if (!usb) oled.drawBox(x + 2, y - 5, (pct * 10 + 50) / 100, 3);
  char s[8]; snprintf(s, sizeof s, usb ? "USB" : "%d%%", pct);
  oled.setFont(u8g2_font_helvR08_tf); oled.drawStr(x + 20, y, s);
}

static void drawSignal(int x, int y, int pct) {
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2, bx = x + i * 4;
    if (pct >= 13 + i * 25) oled.drawBox(bx, y - h, 3, h); else oled.drawPixel(bx + 1, y - 1);
  }
  char s[8]; snprintf(s, sizeof s, "%d%%", pct);
  oled.setFont(u8g2_font_helvR08_tf); oled.drawStr(x + 20, y, s);
}

static void drawTemp(int x, int y, int8_t halfC) {
  oled.drawCircle(x + 3, y - 2, 2); oled.drawFrame(x + 2, y - 9, 3, 6);   // thermometer
  char s[12]; snprintf(s, sizeof s, "%d\xC2\xB0" "F", (int)lround(halfC / 2.0 * 9 / 5 + 32));
  oled.setFont(u8g2_font_helvR08_tf); oled.drawUTF8(x + 20, y, s);
}

static void drawDisplay() {
  oled.clearBuffer();
  oled.setFontMode(1);

  if (!radioOk) {
    oled.setFont(u8g2_font_helvB10_tf); drawCentered(30, "Radio failed");
    oled.setFont(u8g2_font_helvR08_tf); drawCentered(46, "check the antenna, restart");
  } else if (!haveReport) {
    oled.setFont(u8g2_font_helvB10_tf); drawCentered(22, myName);
    oled.setFont(u8g2_font_helvR08_tf); drawCentered(38, "Waiting for collar");
    char f[24]; snprintf(f, sizeof f, "%.1f MHz  SF%u", FREQ_MHZ, SF); drawCentered(52, f);
  } else {
    const Report &r = lastRep.r;
    double lat, lon, dist = 0, bearing = 0;
    bool havePos = collarPosition(lat, lon);
    if (havePos && homeSet) distBearing(homeLat, homeLon, lat, lon, dist, bearing);
    double ft = dist * 3.28084;
    bool atHome = havePos && homeSet && ft < 30;
    drawCompass(havePos && homeSet, bearing, atHome, r.sats);
    if (r.flags & F_FIND) { oled.drawBox(0, 0, 18, 7); oled.setDrawColor(0); oled.setFont(u8g2_font_4x6_tf); oled.drawStr(1, 6, "FIND"); oled.setDrawColor(1); }

    // distance
    char num[12] = "--", unit[4] = "";
    if (atHome) snprintf(num, sizeof num, "Home");
    else if (havePos && homeSet) {
      if (ft < 1000) { snprintf(num, sizeof num, "%d", (int)lround(ft)); strcpy(unit, "ft"); }
      else { double mi = dist / 1609.34; snprintf(num, sizeof num, mi < 10 ? "%.2f" : mi < 100 ? "%.1f" : "%.0f", mi); strcpy(unit, "mi"); }
    }
    oled.setFont(u8g2_font_helvB14_tf); oled.drawStr(COL_X, 16, num);
    int w = oled.getStrWidth(num);
    oled.setFont(u8g2_font_helvB08_tf); oled.drawStr(COL_X + w + 2, 16, unit);

    // direction and age of the position
    char line[24], age[8];
    fmtAge((millis() - lastRep.atMs) / 1000 + (uint32_t)r.age_min * 60, age, sizeof age);
    if (!havePos) snprintf(line, sizeof line, "no GPS fix yet");
    else if (!homeSet) snprintf(line, sizeof line, "hold PRG: set home");
    else if (atHome) snprintf(line, sizeof line, "%s ago", age);
    else snprintf(line, sizeof line, "%s \xC2\xB7 %s ago", compass8(bearing), age);
    oled.setFont(u8g2_font_helvR08_tf); oled.drawUTF8(COL_X, 28, line);
    oled.drawHLine(COL_X, 33, 128 - COL_X);

    drawBattery(COL_X, 43, battPct(r.batt_mv), r.flags & F_USB);
    drawSignal(COL_X, 53, signalPct(lastRep.rssi, lastRep.snr));
    if (lastRep.temp_halfc != TEMP_UNKNOWN) drawTemp(COL_X, 63, lastRep.temp_halfc);
  }

  if (noticeMsg[0] && (int32_t)(noticeUntil - millis()) > 0) {
    oled.setDrawColor(1); oled.drawRBox(4, 50, 120, 14, 3);
    oled.setDrawColor(0); oled.setFont(u8g2_font_helvB08_tf); drawCentered(60, noticeMsg);
    oled.setDrawColor(1);
  }
  oled.sendBuffer();
}

// Short press: find mode for BUTTON_FIND_MIN. Hold 2 s: set home from the collar's last position.
static void pollButton() {
  static bool wasDown = false, longDone = false; static uint32_t downAt = 0;
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down && !wasDown) { downAt = millis(); longDone = false; }
  if (down && !longDone && millis() - downAt > 2000) { longDone = true; if (setHomeFromCollar()) emitHome(true, true); drawDisplay(); }
  if (!down && wasDown && !longDone && millis() - downAt > 40) {
    if (queueAdd(Q_CMD, CMD_FIND, BUTTON_FIND_MIN, nullptr) >= 0) notice("Find queued");
  }
  wasDown = down;
}

// ================= main =================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never stall the radio loop when no serial monitor is attached
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
  delay(100);
  oled.begin();

  myId = (uint32_t)(ESP.getEfuseMac() >> 16);
  prefs.begin("cattrack", false);
  String n = prefs.getString("name", "");
  if (n.length()) snprintf(myName, sizeof myName, "%s", n.c_str());
  else snprintf(myName, sizeof myName, "Base-%04lx", (unsigned long)(myId & 0xFFFF));
  loadHome();
  snprintf(bleName, sizeof bleName, "CatBase-%04lx", (unsigned long)(myId & 0xFFFF));
  drawDisplay();

  uint32_t t = millis();
  while (!Serial && millis() - t < 1500) delay(10);
  Serial.printf("# %s starting, id %08lx, name %s\n", FW_VERSION, (unsigned long)myId, myName);

  nextCmdSeq = (esp_random() % 250) + 1;
  randomSeed(esp_random());
  bleBegin();
  radioOk = radioBegin();
  nextHelloMs = millis() + random(2000, 6000);
  drawDisplay();
}

void loop() {
  static uint32_t lastDraw = 0;

  while (Serial.available()) feedLine(usbLine, Serial.read(), true);
  char tmp[64]; size_t got;
  while ((got = xStreamBufferReceive(bleIn, tmp, sizeof tmp, 0)) > 0)
    for (size_t i = 0; i < got; i++) feedLine(bleLine, tmp[i], false);
  pollButton();

  if (rxFlag && radioOk) { rxFlag = false; handlePacket(); drawDisplay(); lastDraw = millis(); }

  if (awaitingReply && (int32_t)(millis() - replyDeadline) >= 0) {
    awaitingReply = false;
    QItem *h = queueHead();
    if (h) emitCmdState(*h, "waiting");   // will be resent after the collar's next report
  }

  // stream history to the app a few entries at a time
  if (histReplayLeft && bleClient) {
    for (int k = 0; k < 3 && histReplayLeft; k++, histReplayLeft--) {
      const HistEntry &e = hist[(histHead + HISTORY_LEN - histReplayLeft) % HISTORY_LEN];
      JsonDocument d; reportJson(d, e, true); emit(d, false, true);
    }
    if (!histReplayLeft) {
      if (nodesReplayPending) { for (size_t i = 0; i < nodeCount; i++) emitNode(nodes[i], false); nodesReplayPending = false; }
      if (collarCfgKnown) { JsonDocument d; d["evt"] = "collar_config"; configToJson(collarCfg, d["config"].to<JsonObject>());
                            d["ago_ms"] = millis() - collarCfgAtMs; emit(d, false, true); }
      JsonDocument d; d["evt"] = "history_end"; emit(d, false, true);
    }
  }

  // chat bookkeeping
  for (auto &p : pendingAcks) if (p.used && millis() - p.sentMs > TEXT_ACK_WAIT_MS) {
    p.used = false;
    JsonDocument d; d["evt"] = "text_unconfirmed"; d["msg_id"] = p.msgId; emit(d);
  }
  if (radioOk && (int32_t)(millis() - nextHelloMs) >= 0 && !awaitingReply) {
    sendHello();
    nextHelloMs = millis() + HELLO_EVERY_MS + random(0, 30000);
  }

  if (millis() - lastDraw > 1000) { drawDisplay(); lastDraw = millis(); }
  delay(2);
}
