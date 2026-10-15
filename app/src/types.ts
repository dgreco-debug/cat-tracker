// Shapes of the JSON lines exchanged with the base and collar firmware (see PROTOCOL.md).

export interface CollarConfig {
  interval_s: number;
  find_interval_s: number;
  low_batt_mv: number;
  fix_timeout_s: number;
  first_fix_timeout_s: number;
  rx_window_ms: number;
  tx_dbm: number;
  good_hacc_m: number;
  ble_minutes: number;
  leds: boolean;
}

export const DEFAULT_CONFIG: CollarConfig = {
  interval_s: 300, find_interval_s: 30, low_batt_mv: 3500, fix_timeout_s: 60, first_fix_timeout_s: 120,
  rx_window_ms: 2000, tx_dbm: 17, good_hacc_m: 20, ble_minutes: 5, leds: true,
};

export const FLAGS = { FIX: 0x01, LOWBAT: 0x02, FIND: 0x04, GPS_FAIL: 0x08, USB: 0x10, BOOT: 0x20, BLE: 0x40 };

/** One collar report, stamped with the time the app believes it was sent. */
export interface ReportPoint {
  t: number;             // epoch ms
  seq: number;
  fix: boolean;
  lat?: number;
  lon?: number;
  hacc_m: number;
  sats: number;
  gps_s: number;
  batt_mv: number;
  interval_s: number;
  age_min: number;
  flags: number;
  rssi?: number;
  snr?: number;
  temp_c?: number;       // collar temperature (collar 2.2+)
  via: 'base' | 'collar';
}

export type CmdState = 'queued' | 'sent' | 'waiting' | 'done' | 'rejected' | 'cancelled';

export interface QueueItem { qid: number; action: string; arg?: number; tries: number; }

export interface ChatMessage {
  key: string;           // local unique id
  t: number;
  mine: boolean;
  from: string;          // node id (hex) or 'me'
  name: string;
  text: string;
  msgId?: number;
  status?: 'sending' | 'sent' | 'delivered' | 'unconfirmed' | 'error';
  deliveredTo?: string[];
  rssi?: number;
}

export interface NodeInfo { id: string; name: string; rssi?: number; snr?: number; lastSeen: number; }

export type LogKind = 'report' | 'cmd' | 'chat' | 'device' | 'error' | 'info';
export interface LogEntry { t: number; kind: LogKind; text: string; }

export interface DeviceInfo {
  dev: 'base' | 'collar';
  fw: string;
  id: string;
  name: string;
}

export type Incoming = { evt: string; [k: string]: any };
