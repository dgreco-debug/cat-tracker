// App state: connections, collar reports, config, command queue, chat, log. Small observable store.
import { useEffect, useState } from 'preact/hooks';
import { connectBle, connectSerial, type Link, type LinkKind, type Role } from './link';
import { connectDemo } from './demo';
import { fmtTemp, signalPercent } from './geo';
import type { ChatMessage, CmdState, CollarConfig, Incoming, LogEntry, LogKind, NodeInfo, QueueItem, ReportPoint } from './types';

export type ConnStatus = 'disconnected' | 'connecting' | 'connected';

export interface Conn {
  status: ConnStatus;
  link?: Link;
  name?: string;
  fw?: string;
  id?: string;
  error?: string;
}

export interface CollarLive {          // what the collar reports about itself over a direct link
  radio?: boolean; gps?: boolean; gps_baud?: number; batt_mv?: number; usb?: boolean; find?: boolean;
  next_s?: number; uptime_s?: number; reset?: string; temp_c?: number; t: number;
}

export interface Settings {
  layer: 'street' | 'satellite';
  trailHours: number;      // 0 = all
  followCat: boolean;
  showMe: boolean;
}

export interface State {
  base: Conn;
  collar: Conn;
  baseInfo: { freq_mhz?: number; sf?: number; net_id?: number; text_max?: number; home?: { lat: number; lon: number } };
  syncing: boolean;
  points: ReportPoint[];
  config?: { value: CollarConfig; t: number; source: 'base' | 'collar' };
  collarLive?: CollarLive;
  queue: QueueItem[];
  cmdLog: { qid: number; action: string; arg?: number; state: CmdState; t: number }[];
  chat: ChatMessage[];
  nodes: NodeInfo[];
  log: LogEntry[];
  me?: { lat: number; lon: number; acc: number; t: number };
  toast?: { text: string; t: number; kind: 'info' | 'error' };
  settings: Settings;
}

const load = <T,>(key: string, fallback: T): T => {
  try { const v = localStorage.getItem(key); return v ? (JSON.parse(v) as T) : fallback; } catch { return fallback; }
};

let state: State = {
  base: { status: 'disconnected' },
  collar: { status: 'disconnected' },
  baseInfo: {},
  syncing: false,
  points: load<ReportPoint[]>('ct.points', []),
  config: load<State['config']>('ct.config', undefined),
  queue: [],
  cmdLog: [],
  chat: load<ChatMessage[]>('ct.chat', []),
  nodes: load<NodeInfo[]>('ct.nodes', []),
  log: load<LogEntry[]>('ct.log', []),
  settings: { layer: 'street', trailHours: 6, followCat: true, showMe: true, ...load<Partial<Settings>>('ct.settings', {}) },
};

const listeners = new Set<() => void>();
let saveTimer: number | undefined;

function set(patch: Partial<State> | ((s: State) => Partial<State>)) {
  const p = typeof patch === 'function' ? patch(state) : patch;
  state = { ...state, ...p };
  listeners.forEach((l) => l());
  window.clearTimeout(saveTimer);
  saveTimer = window.setTimeout(persist, 500);
}

function persist() {
  try {
    localStorage.setItem('ct.points', JSON.stringify(state.points.slice(-5000)));
    localStorage.setItem('ct.config', JSON.stringify(state.config ?? null));
    localStorage.setItem('ct.chat', JSON.stringify(state.chat.slice(-500)));
    localStorage.setItem('ct.nodes', JSON.stringify(state.nodes));
    localStorage.setItem('ct.log', JSON.stringify(state.log.slice(-400)));
    localStorage.setItem('ct.settings', JSON.stringify(state.settings));
  } catch { /* storage full or blocked: keep running in memory */ }
}

export const getState = () => state;

export function useStore<T>(select: (s: State) => T): T {
  const [v, setV] = useState(() => select(state));
  useEffect(() => {
    const l = () => setV(() => select(state));
    listeners.add(l);
    l();
    return () => { listeners.delete(l); };
  }, []);
  return v;
}

// ---------------- helpers ----------------
function addLog(kind: LogKind, text: string, t = Date.now()) {
  set((s) => ({ log: [...s.log.slice(-399), { t, kind, text }] }));
}

export function toast(text: string, kind: 'info' | 'error' = 'info') {
  set({ toast: { text, t: Date.now(), kind } });
  window.setTimeout(() => { if (state.toast && Date.now() - state.toast.t >= 3900) set({ toast: undefined }); }, 4000);
}

const describeAction = (action: string, arg?: number) => {
  switch (action) {
    case 'find': return `Find mode ${arg ?? ''} min`;
    case 'sleep': return `Sleep ${arg ?? ''} min`;
    case 'now': return 'Report now';
    case 'interval': return `Interval ${arg ?? ''} s`;
    case 'power': return `Power ${arg ?? ''} dBm`;
    case 'config': return 'Settings update';
    case 'get_config': return 'Read settings';
    default: return action;
  }
};
export { describeAction };

function addPoint(m: Incoming, via: 'base' | 'collar') {
  const t = Date.now() - (m.ago_ms ?? 0);
  const p: ReportPoint = {
    t, seq: m.seq, fix: !!m.fix, lat: m.lat, lon: m.lon, hacc_m: m.hacc_m, sats: m.sats, gps_s: m.gps_s,
    batt_mv: m.batt_mv, interval_s: m.interval_s, age_min: m.age_min, flags: m.flags, rssi: m.rssi, snr: m.snr,
    temp_c: m.temp_c, via,
  };
  set((s) => {
    if (s.points.some((q) => q.seq === p.seq && Math.abs(q.t - p.t) < 90_000)) return {};
    const points = [...s.points, p].sort((a, b) => a.t - b.t).slice(-5000);
    return { points };
  });
  if (!m.hist) {
    const where = p.lat !== undefined ? `${p.lat.toFixed(5)}, ${p.lon!.toFixed(5)}` : 'no position';
    addLog('report', `Check-in #${p.seq}: ${p.fix ? '' : 'no fix, last known '}${where}, ±${p.hacc_m} m, battery ${(p.batt_mv / 1000).toFixed(2)} V` +
      (p.rssi !== undefined ? `, signal ${signalPercent(p.rssi, p.snr)}%` : '') +
      (p.temp_c !== undefined ? `, ${fmtTemp(p.temp_c)}` : ''), t);
    if (m.reset) addLog('device', `Collar restarted before check-in #${p.seq}: ${m.reset}`, t);
  }
}

// ---------------- incoming messages ----------------
function onMessage(role: Role, m: Incoming) {
  switch (m.evt) {
    case 'hello':
      set((s) => ({ [role]: { ...s[role], name: m.name, fw: m.fw, id: m.id } } as Partial<State>));
      if (role === 'base') {
        set({ baseInfo: { freq_mhz: m.freq_mhz, sf: m.sf, net_id: m.net_id, text_max: m.text_max, home: m.home } });
        if (m.collar_config) set({ config: { value: m.collar_config, t: Date.now() - (m.collar_config_ago_ms ?? 0), source: 'base' } });
        set({ syncing: true });
        void send('base', { cmd: 'history' });
      } else {
        onCollarStatus(m);
      }
      break;
    case 'status': onCollarStatus(m); break;
    case 'history_end': set({ syncing: false }); break;
    case 'report': addPoint(m, m.local ? 'collar' : role); break;
    case 'collar_config':
    case 'config':
      set({ config: { value: m.config, t: Date.now() - (m.ago_ms ?? 0), source: m.evt === 'config' ? 'collar' : 'base' } });
      if (m.saved) { toast('Saved on the collar'); addLog('cmd', 'Collar saved new settings (Bluetooth)'); }
      break;
    case 'queue': set({ queue: m.items ?? [] }); break;
    case 'cmd': {
      const entry = { qid: m.qid, action: m.action, arg: m.arg, state: m.state as CmdState, t: Date.now() };
      set((s) => ({ cmdLog: [...s.cmdLog.filter((c) => c.qid !== m.qid), entry].slice(-50) }));
      const words: Record<string, string> = {
        queued: 'queued, goes out after the next check-in', sent: 'sent to collar', waiting: 'no reply yet, will retry next check-in',
        done: 'confirmed by collar', rejected: 'rejected by collar (out of range)', cancelled: 'cancelled',
      };
      addLog('cmd', `${describeAction(m.action, m.arg)}: ${words[m.state] ?? m.state}`);
      if (m.state === 'done') toast(`${describeAction(m.action, m.arg)} confirmed`);
      if (m.state === 'rejected') toast(`${describeAction(m.action, m.arg)} rejected`, 'error');
      break;
    }
    case 'action':
      addLog('cmd', `${describeAction(m.action)} ${m.ok ? 'applied on collar' : 'refused by collar'}`);
      if (m.ok) toast(`${describeAction(m.action)} applied`);
      break;
    case 'text': {
      set((s) => {
        if (s.chat.some((c) => !c.mine && c.from === m.from && c.msgId === m.msg_id)) return {};
        const msg: ChatMessage = { key: `${m.from}-${m.msg_id}-${Date.now()}`, t: Date.now(), mine: false, from: m.from,
                                   name: m.name, text: m.text, msgId: m.msg_id, rssi: m.rssi };
        return { chat: [...s.chat, msg].slice(-500) };
      });
      upsertNode({ id: m.from, name: m.name, rssi: m.rssi, snr: m.snr, lastSeen: Date.now() });
      addLog('chat', `${m.name}: ${m.text}`);
      break;
    }
    case 'text_sent':
      set((s) => ({ chat: s.chat.map((c) => (c.key === m.cid ? { ...c, msgId: m.msg_id, status: 'sent' } : c)) }));
      break;
    case 'text_ack':
      set((s) => ({ chat: s.chat.map((c) => (c.mine && c.msgId === m.msg_id
        ? { ...c, status: 'delivered', deliveredTo: Array.from(new Set([...(c.deliveredTo ?? []), m.by_name])) } : c)) }));
      break;
    case 'text_unconfirmed':
      set((s) => ({ chat: s.chat.map((c) => (c.mine && c.msgId === m.msg_id && c.status === 'sent' ? { ...c, status: 'unconfirmed' } : c)) }));
      break;
    case 'home':
      set((s) => ({ baseInfo: { ...s.baseInfo, home: m.set ? { lat: m.lat, lon: m.lon } : undefined } }));
      toast(m.set ? 'Base location saved' : 'Base location cleared');
      addLog('device', m.set ? 'Base location set (used for distance and direction on the base screen)' : 'Base location cleared');
      break;
    case 'node':
      upsertNode({ id: m.id, name: m.name, rssi: m.rssi, snr: m.snr, lastSeen: Date.now() - (m.ago_ms ?? 0) });
      break;
    case 'dfu': toast('Collar restarted into update mode: copy the new .uf2 onto the XIAO drive'); break;
    case 'log': addLog('device', `${role === 'base' ? 'Base' : 'Collar'}: ${m.msg}`); break;
    case 'error':
      addLog('error', `${role === 'base' ? 'Base' : 'Collar'}: ${m.msg}`);
      toast(m.msg, 'error');
      break;
  }
}

function onCollarStatus(m: Incoming) {
  set((s) => ({
    collar: { ...s.collar, name: m.name ?? s.collar.name, fw: m.fw ?? s.collar.fw, id: m.id ?? s.collar.id },
    collarLive: { radio: m.radio, gps: m.gps, gps_baud: m.gps_baud, batt_mv: m.batt_mv, usb: m.usb, find: m.find,
                  next_s: m.next_s, uptime_s: m.uptime_s, reset: m.reset, temp_c: m.temp_c, t: Date.now() },
    ...(m.config ? { config: { value: m.config, t: Date.now(), source: 'collar' as const } } : {}),
  }));
  if (m.last_fix) {
    const lf = m.last_fix;
    addPoint({ evt: 'report', seq: m.seq ?? -1, fix: true, lat: lf.lat, lon: lf.lon, hacc_m: lf.hacc_m, sats: lf.sats, gps_s: 0,
               batt_mv: m.batt_mv, interval_s: m.config?.interval_s ?? 0, age_min: Math.floor(lf.age_s / 60), flags: 1,
               ago_ms: lf.age_s * 1000, hist: true }, 'collar');
  }
}

function upsertNode(n: NodeInfo) {
  set((s) => {
    const others = s.nodes.filter((x) => x.id !== n.id);
    const prev = s.nodes.find((x) => x.id === n.id);
    return { nodes: [...others, { ...prev, ...n, lastSeen: Math.max(n.lastSeen, prev?.lastSeen ?? 0) }] };
  });
}

// ---------------- actions ----------------
export async function connect(role: Role, kind: LinkKind) {
  if (state[role].link) await disconnect(role);
  set({ [role]: { status: 'connecting' } } as Partial<State>);
  const handlers = {
    onMessage: (m: Incoming) => onMessage(role, m),
    onRaw: (line: string) => { if (!line.startsWith('[')) return; addLog('device', `${role === 'base' ? 'Base' : 'Collar'}: ${line}`); },
    onClose: (reason?: string) => {
      set({ [role]: { status: 'disconnected', error: reason } } as Partial<State>);
      if (role === 'base') set({ syncing: false, queue: [] });
      addLog('info', `${role === 'base' ? 'Base' : 'Collar'} disconnected${reason ? `: ${reason}` : ''}`);
    },
  };
  try {
    const link = kind === 'ble' ? await connectBle(role, handlers) : kind === 'serial' ? await connectSerial(role, handlers)
                                : connectDemo(role, handlers);
    set({ [role]: { status: 'connected', link, name: link.label } } as Partial<State>);
    addLog('info', `Connected to ${link.label} (${kind === 'ble' ? 'Bluetooth' : kind === 'serial' ? 'USB' : 'demo'})`);
    await link.send({ cmd: 'hello' });
  } catch (e: any) {
    const msg = e?.name === 'NotFoundError' ? 'No device chosen' : (e?.message ?? String(e));
    set({ [role]: { status: 'disconnected', error: msg } } as Partial<State>);
    if (e?.name !== 'NotFoundError') toast(msg, 'error');
  }
}

export async function disconnect(role: Role) {
  const l = state[role].link;
  set({ [role]: { status: 'disconnected' } } as Partial<State>);
  if (l) await l.close();
}

export async function send(role: Role, obj: object) {
  const l = state[role].link;
  if (!l) { toast(`${role === 'base' ? 'Base' : 'Collar'} is not connected`, 'error'); return false; }
  try { await l.send(obj); return true; } catch (e: any) { toast(`Send failed: ${e?.message ?? e}`, 'error'); return false; }
}

/** Sends an action to the collar: directly if connected over Bluetooth/USB, otherwise through the base over LoRa. */
export function collarAction(action: 'find' | 'sleep' | 'now' | 'interval' | 'power' | 'get_config', arg?: number) {
  if (state.collar.link) {
    if (action === 'get_config') return send('collar', { cmd: 'get_config' });
    if (action === 'interval' || action === 'power') {
      return send('collar', { cmd: 'set_config', config: action === 'interval' ? { interval_s: arg } : { tx_dbm: arg } });
    }
    return send('collar', { cmd: 'action', action, arg });
  }
  return send('base', { cmd: 'collar', action, arg });
}

/** Saves the collar's last reported position as the base's location (for distance and direction on its screen). */
export function setBaseHome() {
  return send('base', { cmd: 'set_home' });
}

export function sendConfig(cfg: CollarConfig) {
  if (state.collar.link) return send('collar', { cmd: 'set_config', config: cfg });
  return send('base', { cmd: 'collar_config', config: cfg });
}

/** Restarts a directly connected collar into its UF2 bootloader (an XIAO drive appears on the computer). */
export function collarUpdateMode() { return send('collar', { cmd: 'dfu' }); }

export function cancelQueued(qid?: number) { return send('base', { cmd: 'cancel', qid: qid ?? 0 }); }

export async function sendChat(text: string) {
  const key = `me-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`;
  const msg: ChatMessage = { key, t: Date.now(), mine: true, from: 'me', name: state.base.name ?? 'Me', text, status: 'sending' };
  set((s) => ({ chat: [...s.chat, msg].slice(-500) }));
  const ok = await send('base', { cmd: 'send_text', text, cid: key });
  if (!ok) set((s) => ({ chat: s.chat.map((c) => (c.key === key ? { ...c, status: 'error' } : c)) }));
}

export function setBaseName(name: string) { return send('base', { cmd: 'set_name', name }); }

export function updateSettings(p: Partial<Settings>) { set((s) => ({ settings: { ...s.settings, ...p } })); }

export function clearTrail() { set({ points: [] }); addLog('info', 'Track history cleared'); }
export function clearLog() { set({ log: [] }); }

let watchId: number | undefined;
export function watchMe(on: boolean) {
  if (!('geolocation' in navigator)) return;
  if (on && watchId === undefined) {
    watchId = navigator.geolocation.watchPosition(
      (p) => set({ me: { lat: p.coords.latitude, lon: p.coords.longitude, acc: p.coords.accuracy, t: Date.now() } }),
      () => { /* permission denied or unavailable */ },
      { enableHighAccuracy: true, maximumAge: 10_000 },
    );
  } else if (!on && watchId !== undefined) {
    navigator.geolocation.clearWatch(watchId);
    watchId = undefined;
    set({ me: undefined });
  }
}
