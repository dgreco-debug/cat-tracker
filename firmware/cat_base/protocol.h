// Cat tracker radio protocol, version 2. Keep this file identical in the collar and base sketches.
//
// Every packet starts with MAGIC, NET_ID, type. Packets stay at or under 24 bytes so each one is
// under ~370 ms on air at SF10/125 kHz (the US 400 ms dwell limit).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace cat {

// ---------- radio settings (must match on every device) ----------
constexpr float    FREQ_MHZ  = 915.0;
constexpr float    BW_KHZ    = 125.0;
constexpr uint8_t  SF        = 10;
constexpr uint8_t  CR        = 5;      // 4/5
constexpr uint8_t  SYNC_WORD = 0x1D;   // private: differs from Meshtastic (0x2B) and LoRaWAN (0x34)
constexpr uint16_t PREAMBLE  = 8;

constexpr uint8_t MAGIC  = 0xC7;
constexpr uint8_t NET_ID = 0x42;       // change on every device if a neighbour runs the same firmware
constexpr size_t  MAX_PACKET = 24;

enum PacketType : uint8_t {
  // collar -> base
  PKT_REPORT     = 0x01,
  PKT_ACK        = 0x02,
  PKT_CONFIG     = 0x03,  // collar's current config (reply to SET_CONFIG or CMD_GET_CONFIG)
  // base -> collar
  PKT_CMD        = 0x10,
  PKT_SET_CONFIG = 0x11,
  // base <-> base (chat)
  PKT_TEXT       = 0x20,
  PKT_TEXT_ACK   = 0x21,
  PKT_HELLO      = 0x22,
};

enum Command : uint8_t {
  CMD_INTERVAL   = 1,  // arg = seconds between reports (saved)
  CMD_SLEEP      = 2,  // arg = minutes of radio silence, then report and resume
  CMD_FIND       = 3,  // arg = minutes of fast reporting
  CMD_POWER      = 4,  // arg = transmit power, dBm (saved)
  CMD_NOW        = 5,  // report again right away
  CMD_GET_CONFIG = 6,  // collar replies with PKT_CONFIG
};

enum ReportFlags : uint8_t {
  F_FIX      = 0x01,  // position is from this report's fix (else last known)
  F_LOWBAT   = 0x02,
  F_FIND     = 0x04,
  F_GPS_FAIL = 0x08,
  F_USB      = 0x10,
  F_BOOT     = 0x20,
  F_BLE      = 0x40,  // collar Bluetooth is advertising or connected
};

// Why the collar last restarted. Sent in Report.last_cmd on the first report after power-up (F_BOOT set),
// when there is no applied command yet. 0 means power-on or brownout (the nRF52 records no other cause).
enum ResetReason : uint8_t {
  RR_PIN = 0x01, RR_WATCHDOG = 0x02, RR_SOFTWARE = 0x04, RR_LOCKUP = 0x08, RR_WAKE = 0x10, RR_VBUS = 0x20,
};

// Packs NRF_POWER->RESETREAS into a ResetReason bit set.
inline uint8_t resetReasonCode(uint32_t resetreas) {
  return (uint8_t)((resetreas & 0x0F) | (((resetreas >> 16) & 1) << 4) | (((resetreas >> 20) & 1) << 5));
}

inline const char *resetReasonName(uint8_t code) {
  if (code & RR_LOCKUP)   return "crash (CPU lockup)";
  if (code & RR_WATCHDOG) return "watchdog";
  if (code & RR_SOFTWARE) return "software reset (firmware restart or crash handler)";
  if (code & RR_PIN)      return "reset button";
  if (code & RR_VBUS)     return "USB power connected";
  if (code & RR_WAKE)     return "wake from sleep";
  return "power-on or brownout";
}

enum AckStatus : uint8_t { ACK_APPLIED = 0, ACK_DUPLICATE = 1, ACK_REJECTED = 2 };

enum ConfigFlags : uint8_t { CF_LEDS = 0x01 };

#pragma pack(push, 1)

struct CollarConfig {          // 16 bytes, stored on the collar and sent over the air
  uint32_t interval_s;          // 15..86400
  uint16_t find_interval_s;     // 10..600
  uint16_t low_batt_mv;         // 3000..4000
  uint8_t  fix_timeout_s;       // 10..250
  uint8_t  first_fix_timeout_s; // 30..250
  uint8_t  rx_window_ds;        // listen window after a report, in 100 ms units, 5..50
  int8_t   tx_dbm;              // -9..22
  uint8_t  good_hacc_m;         // stop the GPS early once accuracy is this good, 3..100
  uint8_t  ble_minutes;         // Bluetooth stays on this long after boot or button press, 1..60
  uint8_t  flags;               // ConfigFlags
  uint8_t  version;             // CONFIG_VERSION
};
constexpr uint8_t CONFIG_VERSION = 1;

inline CollarConfig defaultConfig() {
  CollarConfig c{};
  c.interval_s = 300; c.find_interval_s = 30; c.low_batt_mv = 3500;
  c.fix_timeout_s = 60; c.first_fix_timeout_s = 120; c.rx_window_ds = 20; c.tx_dbm = 22;
  c.good_hacc_m = 20; c.ble_minutes = 5; c.flags = CF_LEDS; c.version = CONFIG_VERSION;
  return c;
}

// Returns true if every field is within range.
inline bool configValid(const CollarConfig &c) {
  return c.interval_s >= 15 && c.interval_s <= 86400 &&
         c.find_interval_s >= 10 && c.find_interval_s <= 600 &&
         c.low_batt_mv >= 3000 && c.low_batt_mv <= 4000 &&
         c.fix_timeout_s >= 10 && c.fix_timeout_s <= 250 &&
         c.first_fix_timeout_s >= 30 && c.first_fix_timeout_s <= 250 &&
         c.rx_window_ds >= 5 && c.rx_window_ds <= 50 &&
         c.tx_dbm >= -9 && c.tx_dbm <= 22 &&
         c.good_hacc_m >= 3 && c.good_hacc_m <= 100 &&
         c.ble_minutes >= 1 && c.ble_minutes <= 60;
}

struct Report {                // collar -> base, 24 bytes
  uint8_t  magic, net, type;
  uint16_t seq;
  uint8_t  flags;
  int32_t  lat_e7, lon_e7;
  uint16_t hacc_m;
  uint8_t  sats;
  uint16_t batt_mv;
  uint8_t  fix_s;
  uint8_t  last_cmd;           // last applied command seq; ResetReason instead when F_BOOT is set
  uint16_t interval_s;         // current interval, capped at 65535
  uint8_t  age_min;
};

struct Cmd {                   // base -> collar, 9 bytes
  uint8_t  magic, net, type;
  uint8_t  cmd_seq, code;
  uint32_t arg;
};

struct Ack {                   // collar -> base, 6 bytes
  uint8_t  magic, net, type;
  uint8_t  cmd_seq, code, status;
};

struct SetConfig {             // base -> collar, 20 bytes
  uint8_t  magic, net, type;
  uint8_t  cmd_seq;
  CollarConfig cfg;
};

struct ConfigReply {           // collar -> base, 21 bytes
  uint8_t  magic, net, type;
  uint8_t  cmd_seq, status;
  CollarConfig cfg;
};

constexpr size_t TEXT_FRAG_BYTES = 14;
constexpr uint8_t TEXT_MAX_FRAGS = 8;
constexpr size_t TEXT_MAX_BYTES = TEXT_FRAG_BYTES * TEXT_MAX_FRAGS;  // 112 bytes of UTF-8

struct TextFrag {              // base -> all, up to 24 bytes (last fragment may be shorter)
  uint8_t  magic, net, type;
  uint32_t from;               // sender node id
  uint16_t msg_id;
  uint8_t  frag;               // high nibble = index (0-based), low nibble = total count
  char     text[TEXT_FRAG_BYTES];
};
constexpr size_t TEXT_HEADER = 10;

struct TextAck {               // receiver -> all, 13 bytes
  uint8_t  magic, net, type;
  uint32_t from;               // who received it
  uint32_t to;                 // original sender
  uint16_t msg_id;
};

constexpr size_t NODE_NAME_MAX = 16;
struct Hello {                 // base -> all, 7 + name length (max 23)
  uint8_t  magic, net, type;
  uint32_t id;
  char     name[NODE_NAME_MAX];     // not null-terminated; length = packet length - 7
};
constexpr size_t HELLO_HEADER = 7;

#pragma pack(pop)

static_assert(sizeof(CollarConfig) == 16, "CollarConfig size");
static_assert(sizeof(Report) == 24, "Report size");
static_assert(sizeof(Cmd) == 9, "Cmd size");
static_assert(sizeof(Ack) == 6, "Ack size");
static_assert(sizeof(SetConfig) == 20, "SetConfig size");
static_assert(sizeof(ConfigReply) == 21, "ConfigReply size");
static_assert(sizeof(TextFrag) == 24, "TextFrag size");
static_assert(sizeof(TextAck) == 13, "TextAck size");
static_assert(sizeof(Hello) == 23, "Hello size");

inline bool headerOk(const uint8_t *buf, size_t len) {
  return len >= 3 && buf[0] == MAGIC && buf[1] == NET_ID;
}

}  // namespace cat
