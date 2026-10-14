// JSON <-> CollarConfig helpers shared by the collar and base firmware (ArduinoJson 7).
#pragma once
#include <ArduinoJson.h>
#include "protocol.h"

namespace cat {

inline void configToJson(const CollarConfig &c, JsonObject o) {
  o["interval_s"] = c.interval_s;
  o["find_interval_s"] = c.find_interval_s;
  o["low_batt_mv"] = c.low_batt_mv;
  o["fix_timeout_s"] = c.fix_timeout_s;
  o["first_fix_timeout_s"] = c.first_fix_timeout_s;
  o["rx_window_ms"] = (uint16_t)c.rx_window_ds * 100;
  o["tx_dbm"] = c.tx_dbm;
  o["good_hacc_m"] = c.good_hacc_m;
  o["ble_minutes"] = c.ble_minutes;
  o["leds"] = (c.flags & CF_LEDS) != 0;
}

// Copies any fields present in `o` onto `c`. Out-of-range values are caught later by configValid().
inline void configMergeJson(CollarConfig &c, JsonObjectConst o) {
  if (o["interval_s"].is<long>())          c.interval_s = (uint32_t)o["interval_s"].as<long>();
  if (o["find_interval_s"].is<long>())     c.find_interval_s = (uint16_t)o["find_interval_s"].as<long>();
  if (o["low_batt_mv"].is<long>())         c.low_batt_mv = (uint16_t)o["low_batt_mv"].as<long>();
  if (o["fix_timeout_s"].is<long>())       c.fix_timeout_s = (uint8_t)constrain(o["fix_timeout_s"].as<long>(), 0L, 255L);
  if (o["first_fix_timeout_s"].is<long>()) c.first_fix_timeout_s = (uint8_t)constrain(o["first_fix_timeout_s"].as<long>(), 0L, 255L);
  if (o["rx_window_ms"].is<long>())        c.rx_window_ds = (uint8_t)constrain(o["rx_window_ms"].as<long>() / 100, 0L, 255L);
  if (o["tx_dbm"].is<long>())              c.tx_dbm = (int8_t)constrain(o["tx_dbm"].as<long>(), -128L, 127L);
  if (o["good_hacc_m"].is<long>())         c.good_hacc_m = (uint8_t)constrain(o["good_hacc_m"].as<long>(), 0L, 255L);
  if (o["ble_minutes"].is<long>())         c.ble_minutes = (uint8_t)constrain(o["ble_minutes"].as<long>(), 0L, 255L);
  if (o["leds"].is<bool>())                c.flags = o["leds"].as<bool>() ? (c.flags | CF_LEDS) : (c.flags & ~CF_LEDS);
  c.version = CONFIG_VERSION;
}

inline const char *configRangeHelp() {
  return "interval_s 15-86400, find_interval_s 10-600, low_batt_mv 3000-4000, fix_timeout_s 10-250, "
         "first_fix_timeout_s 30-250, rx_window_ms 500-5000, tx_dbm -9..22, good_hacc_m 3-100, ble_minutes 1-60";
}

}  // namespace cat
