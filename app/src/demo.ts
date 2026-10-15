// Simulated base station and collar so the app can be tried (and tested) without hardware.
import type { Link, LinkHandlers, Role } from './link';
import { DEFAULT_CONFIG, type CollarConfig } from './types';

const CENTER = { lat: 45.5189, lon: -122.6793 };   // a public square; demo data only
const DEMO_SPEED = 30;                             // demo time runs 30x faster than real time

export function connectDemo(role: Role, h: LinkHandlers): Link {
  let cfg: CollarConfig = { ...DEFAULT_CONFIG };
  let pos = { lat: CENTER.lat + 0.0012, lon: CENTER.lon - 0.0009 };
  let heading = Math.random() * Math.PI * 2;
  let seq = 40;
  let batt = 4010;
  let findLeft = 0;
  let sleepLeft = 0;
  let queue: { qid: number; action: string; arg?: number; config?: Partial<CollarConfig>; tries: number }[] = [];
  let qid = 1;
  let closed = false;
  const emit = (o: object) => { if (!closed) setTimeout(() => h.onMessage(o as any), 30); };

  const step = () => {
    heading += (Math.random() - 0.5) * 1.2;
    const d = 0.00006 + Math.random() * 0.00008;
    pos = { lat: pos.lat + Math.cos(heading) * d, lon: pos.lon + Math.sin(heading) * d * 1.4 };
    if (Math.hypot(pos.lat - CENTER.lat, (pos.lon - CENTER.lon) / 1.4) > 0.003) heading += Math.PI;
  };

  const report = (agoMs = 0, histFlag = false) => {
    step();
    seq++;
    batt = Math.max(3400, batt - Math.round(Math.random() * 3));
    const fix = Math.random() > 0.12;
    return {
      evt: 'report', seq, fix, lat: pos.lat, lon: pos.lon, hacc_m: fix ? 4 + Math.round(Math.random() * 12) : 30,
      sats: fix ? 7 + Math.round(Math.random() * 5) : 3, gps_s: fix ? 2 + Math.round(Math.random() * 4) : 60,
      batt_mv: batt, interval_s: findLeft > 0 ? cfg.find_interval_s : cfg.interval_s, age_min: fix ? 0 : 5,
      flags: (fix ? 1 : 0) | (findLeft > 0 ? 4 : 0), rssi: -88 - Math.round(Math.random() * 25),
      snr: 2 + Math.round(Math.random() * 80) / 10, temp_c: 21 + Math.round(Math.random() * 12) / 2, ago_ms: agoMs, ...(histFlag ? { hist: true } : {}),
    };
  };

  const emitQueue = () => emit({ evt: 'queue', items: queue.map(({ qid, action, arg, tries }) => ({ qid, action, arg, tries })) });

  // Collar check-in: report, then deliver queued commands.
  let timer: number | undefined;
  const schedule = () => {
    const realS = findLeft > 0 ? cfg.find_interval_s : cfg.interval_s;
    timer = window.setTimeout(checkIn, Math.max(1500, (sleepLeft > 0 ? sleepLeft : realS) * 1000 / DEMO_SPEED));
  };
  const checkIn = () => {
    if (closed) return;
    sleepLeft = 0;
    if (findLeft > 0) findLeft -= (cfg.find_interval_s / 60);
    emit(role === 'base' ? report() : { ...report(), local: true });
    if (role === 'base') {
      for (const it of queue) {
        it.tries++;
        emit({ evt: 'cmd', qid: it.qid, state: 'sent', action: it.action, arg: it.arg, tries: it.tries });
        apply(it.action, it.arg, it.config);
        emit({ evt: 'cmd', qid: it.qid, state: 'done', action: it.action, arg: it.arg, tries: it.tries });
        if (it.action === 'config' || it.action === 'get_config') emit({ evt: 'collar_config', config: cfg, ago_ms: 0 });
      }
      if (queue.length) { queue = []; emitQueue(); }
    }
    schedule();
  };

  const apply = (action: string, arg?: number, config?: Partial<CollarConfig>) => {
    if (action === 'find') findLeft = arg || 10;
    if (action === 'sleep') sleepLeft = (arg || 30) * 60;
    if (action === 'interval' && arg) cfg.interval_s = arg;
    if (action === 'power' && arg !== undefined) cfg.tx_dbm = arg;
    if (action === 'config' && config) cfg = { ...cfg, ...config };
    if (action === 'now') { window.clearTimeout(timer); window.setTimeout(checkIn, 800); }
  };

  const neighbor = { id: 'b7e2a913', name: 'Neighbor' };

  const handle = (m: any) => {
    switch (m.cmd) {
      case 'hello':
      case 'status':
        if (role === 'base') {
          emit({ evt: 'hello', dev: 'base', fw: 'cat-base 2.0 (demo)', id: 'd3m0ba5e', name: 'Demo base', uptime_ms: 3600000,
                 radio: true, freq_mhz: 915, sf: 10, bw_khz: 125, net_id: 66, history: 24, text_max: 112, collar_config: cfg,
                 reset: 'power-on', rx_reinits: 0, travel_1h_m: 120, travel_24h_m: 1450 });
          emitQueue();
        } else {
          emit({ evt: m.cmd === 'hello' ? 'hello' : 'status', dev: 'collar', fw: 'cat-collar 2.0 (demo)', id: 'd3m0c011',
                 name: 'CatCollar-c011', radio: true, gps: true, gps_baud: 115200, batt_mv: batt, usb: false, find: findLeft > 0,
                 seq, next_s: 42, uptime_s: 5400, reset: 'power-on or brownout', reset_code: 0, temp_c: 24.5, missed_acks: 0, noack_resets: 0, last_ack_s: 12, config: cfg,
                 last_fix: { lat: pos.lat, lon: pos.lon, hacc_m: 6, sats: 9, age_s: 30 } });
        }
        break;
      case 'history':
        for (let i = 24; i >= 1; i--) emit(report(i * cfg.interval_s * 1000, true));
        emit({ evt: 'node', id: neighbor.id, name: neighbor.name, rssi: -104, snr: 3.5, ago_ms: 120000 });
        emit({ evt: 'collar_config', config: cfg, ago_ms: 600000 });
        emit({ evt: 'history_end' });
        break;
      case 'collar': {
        const it = { qid: qid++, action: m.action, arg: m.arg, tries: 0 };
        queue.push(it);
        emit({ evt: 'cmd', qid: it.qid, state: 'queued', action: it.action, arg: it.arg, tries: 0 });
        emitQueue();
        break;
      }
      case 'collar_config': {
        const it = { qid: qid++, action: 'config', config: m.config, tries: 0 };
        queue = queue.filter((q) => q.action !== 'config');
        queue.push(it);
        emit({ evt: 'cmd', qid: it.qid, state: 'queued', action: 'config', tries: 0 });
        emitQueue();
        break;
      }
      case 'cancel':
        queue.filter((q) => !m.qid || q.qid === m.qid).forEach((q) => emit({ evt: 'cmd', qid: q.qid, state: 'cancelled', action: q.action, arg: q.arg, tries: q.tries }));
        queue = queue.filter((q) => m.qid && q.qid !== m.qid);
        emitQueue();
        break;
      case 'send_text': {
        const msgId = Math.floor(Math.random() * 65535);
        emit({ evt: 'text_sent', msg_id: msgId, frags: Math.ceil(m.text.length / 14), cid: m.cid });
        setTimeout(() => emit({ evt: 'text_ack', msg_id: msgId, by: neighbor.id, by_name: neighbor.name, rssi: -101 }), 1800);
        if (Math.random() < 0.6) setTimeout(() => emit({ evt: 'text', from: neighbor.id, name: neighbor.name, msg_id: msgId + 1,
          text: ['Got it 👍', 'She was by the shed a minute ago', 'On my way', 'Copy that'][Math.floor(Math.random() * 4)], rssi: -102, snr: 4 }), 4200);
        break;
      }
      case 'set_name':
        emit({ evt: 'hello', dev: 'base', fw: 'cat-base 2.0 (demo)', id: 'd3m0ba5e', name: m.name, uptime_ms: 3600000, radio: true,
               freq_mhz: 915, sf: 10, bw_khz: 125, net_id: 66, history: 24, text_max: 112 });
        break;
      case 'set_home':
        emit({ evt: 'home', set: !m.clear, lat: m.lat ?? pos.lat, lon: m.lon ?? pos.lon });
        break;
      case 'get_config':
        emit({ evt: 'config', config: cfg });
        break;
      case 'set_config':
        cfg = { ...cfg, ...m.config };
        emit({ evt: 'config', config: cfg, saved: true });
        break;
      case 'reset_config':
        cfg = { ...DEFAULT_CONFIG };
        emit({ evt: 'config', config: cfg, saved: true });
        break;
      case 'action':
        apply(m.action, m.arg);
        emit({ evt: 'action', action: m.action, ok: true });
        break;
      default:
        emit({ evt: 'error', msg: 'unknown cmd' });
    }
  };

  schedule();
  return {
    kind: 'demo',
    label: role === 'base' ? 'Demo base' : 'Demo collar',
    send: async (obj) => handle(obj),
    close: async () => { closed = true; window.clearTimeout(timer); h.onClose('Demo ended'); },
  };
}
