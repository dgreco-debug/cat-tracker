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

#define FW_VERSION "cat-base 2.2"

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
  else if (!strcmp(cmd, "say")) { char *rest = argS ? argS + strlen(argS) + 1 : nullptr; String t = argS ? String(argS) : "";
                                  if (rest && *rest) { t += " "; t += strtok(rest, "\r\n"); } sendText(t.c_str(), ""); }
  else Serial.println("# Commands: interval <s> | find <min> | sleep <min> | power <dBm> | now | get_config | cancel | say <text> | or JSON lines");
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
static void drawDisplay() {
  char l[6][32];
  for (auto &s : l) s[0] = 0;
  if (!radioOk) snprintf(l[0], 32, "RADIO INIT FAILED");
  else if (!haveReport) {
    snprintf(l[0], 32, "%s", myName);
    snprintf(l[1], 32, "%.1f MHz SF%u", FREQ_MHZ, SF);
    snprintf(l[3], 32, "Waiting for collar...");
  } else {
    const Report &r = lastRep.r;
    bool fix = r.flags & F_FIX;
    uint32_t ago = (millis() - lastRep.atMs) / 1000;
    snprintf(l[0], 32, "#%u %.0fdBm %.1fdB", r.seq, lastRep.rssi, lastRep.snr);
    if (r.lat_e7 || r.lon_e7) snprintf(l[1], 32, "%.6f,%.6f", r.lat_e7 / 1e7, r.lon_e7 / 1e7);
    else snprintf(l[1], 32, "no position yet");
    if (fix) snprintf(l[2], 32, "fix %us %usat +/-%um", r.fix_s, r.sats, r.hacc_m);
    else snprintf(l[2], 32, "NO FIX (%usat) %umin old", r.sats, r.age_min);
    snprintf(l[3], 32, "bat %.2fV every %us", r.batt_mv / 1000.0, r.interval_s);
    snprintf(l[4], 32, "%lu:%02lu ago%s%s", (unsigned long)(ago / 60), (unsigned long)(ago % 60),
             r.flags & F_FIND ? " FIND" : (r.flags & F_LOWBAT ? " LOWBAT" : ""), bleClient ? " [app]" : "");
  }
  if (lastChat[0] && millis() - lastChatMs < 60000) snprintf(l[5], 32, "%s", lastChat);
  else if (statusLine[0]) snprintf(l[5], 32, "%s", statusLine);
  else snprintf(l[5], 32, "PRG = find %lu min", (unsigned long)BUTTON_FIND_MIN);

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < 6; i++) oled.drawStr(0, 10 + i * 10, l[i]);
  oled.sendBuffer();
}

static void pollButton() {
  static bool wasDown = false; static uint32_t downAt = 0;
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down && !wasDown) downAt = millis();
  if (!down && wasDown && millis() - downAt > 40) queueAdd(Q_CMD, CMD_FIND, BUTTON_FIND_MIN, nullptr);
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
