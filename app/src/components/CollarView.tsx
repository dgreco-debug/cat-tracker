import { useEffect, useMemo, useState } from 'preact/hooks';
import { cancelQueued, collarAction, collarUpdateMode, describeAction, sendConfig, useStore } from '../store';
import { ago, BATT_CUTOFF_MV, battPercent, fmtInterval } from '../geo';
import { DEFAULT_CONFIG, FLAGS, type CollarConfig, type ReportPoint } from '../types';
import { IconBluetooth, IconRadio } from '../icons';

interface FieldDef {
  key: keyof CollarConfig;
  label: string;
  help: string;
  unit: string;
  toUi: (v: number) => number;
  fromUi: (v: number) => number;
  min: number; max: number; step: number;
}

const FIELDS: FieldDef[] = [
  { key: 'interval_s', label: 'Check in every', help: 'How often the collar gets a GPS fix and reports. Longer saves battery.',
    unit: 'min', toUi: (v) => +(v / 60).toFixed(2), fromUi: (v) => Math.round(v * 60), min: 0.25, max: 1440, step: 0.25 },
  { key: 'find_interval_s', label: 'Find mode check-in', help: 'Faster reporting while you are actively searching.',
    unit: 's', toUi: (v) => v, fromUi: (v) => Math.round(v), min: 10, max: 600, step: 5 },
  { key: 'fix_timeout_s', label: 'GPS search limit', help: 'Give up on a fix after this long and send the last known position.',
    unit: 's', toUi: (v) => v, fromUi: (v) => Math.round(v), min: 10, max: 250, step: 5 },
  { key: 'first_fix_timeout_s', label: 'First fix after power-up', help: 'A cold start needs longer than later fixes.',
    unit: 's', toUi: (v) => v, fromUi: (v) => Math.round(v), min: 30, max: 250, step: 5 },
  { key: 'good_hacc_m', label: 'Good-enough accuracy', help: 'Stop searching early once the fix is at least this precise.',
    unit: 'm', toUi: (v) => v, fromUi: (v) => Math.round(v), min: 3, max: 100, step: 1 },
  { key: 'rx_window_ms', label: 'Listen after check-in', help: 'How long the collar waits for your commands after each report.',
    unit: 'ms', toUi: (v) => v, fromUi: (v) => Math.round(v / 100) * 100, min: 500, max: 5000, step: 100 },
  { key: 'tx_dbm', label: 'Transmit power', help: 'Lower power saves battery but shortens range. 22 is the maximum.',
    unit: 'dBm', toUi: (v) => v, fromUi: (v) => Math.round(v), min: -9, max: 22, step: 1 },
  { key: 'ble_minutes', label: 'Bluetooth after button press', help: 'How long the collar stays visible to this app after power-up or a press of its button.',
    unit: 'min', toUi: (v) => v, fromUi: (v) => Math.round(v), min: 1, max: 60, step: 1 },
  { key: 'low_batt_mv', label: 'Low battery warning', help: 'Reports are flagged below this voltage.',
    unit: 'V', toUi: (v) => v / 1000, fromUi: (v) => Math.round(v * 1000), min: 3.0, max: 4.0, step: 0.05 },
];

const BATTERY_MAH = 100;
const TREND_CHECKINS = 10;
const TREND_MIN_MINUTES = 30;

/** Rough battery life with a BATTERY_MAH cell, from the power budget measured on the bench design. */
function estimateDays(cfg: CollarConfig, avgGpsS: number) {
  const perReportMah = (25 * avgGpsS + 118 * 0.37 * (cfg.tx_dbm >= 20 ? 1 : 0.6) + 6 * (cfg.rx_window_ms / 1000)) / 3600;
  const perDay = (86400 / cfg.interval_s) * perReportMah + 0.02 * 24;
  return (BATTERY_MAH * 0.8) / perDay;
}

/**
 * Days until the battery reaches BATT_CUTOFF_MV, from the trend over recent check-ins on battery.
 * Fits a straight line to state of charge (not raw voltage, which is flat mid-range) against time.
 * Uses at least TREND_CHECKINS check-ins spanning at least TREND_MIN_MINUTES, so short intervals do not
 * turn reading noise into a forecast. Stops at the most recent USB check-in (a charge is not drain) and at a
 * change of check-in interval (the drain rate changes with it).
 */
function daysLeftFromTrend(points: ReportPoint[]): { days?: number; n: number; minutes: number } {
  const pts: ReportPoint[] = [];
  const latest = points[points.length - 1];
  for (let i = points.length - 1; i >= 0; i--) {
    const p = points[i];
    if (p.flags & FLAGS.USB || p.interval_s !== latest.interval_s) break;
    if (p.batt_mv > 2500) pts.unshift(p);
    if (pts.length >= TREND_CHECKINS && pts[pts.length - 1].t - pts[0].t >= TREND_MIN_MINUTES * 60_000) break;
  }
  const minutes = pts.length ? (pts[pts.length - 1].t - pts[0].t) / 60_000 : 0;
  if (pts.length < TREND_CHECKINS || minutes < TREND_MIN_MINUTES) return { n: pts.length, minutes };
  const xs = pts.map((p) => (p.t - pts[0].t) / 3600_000), ys = pts.map((p) => battPercent(p.batt_mv));
  const mx = xs.reduce((a, v) => a + v, 0) / xs.length, my = ys.reduce((a, v) => a + v, 0) / ys.length;
  const sxx = xs.reduce((a, x) => a + (x - mx) ** 2, 0);
  if (sxx === 0) return { n: pts.length, minutes };
  const slope = xs.reduce((a, x, i) => a + (x - mx) * (ys[i] - my), 0) / sxx;   // percent per hour
  if (slope >= 0) return { n: pts.length, minutes };
  const nowPct = Math.max(0, my + slope * (xs[xs.length - 1] - mx));
  return { days: nowPct / -slope / 24, n: pts.length, minutes };
}

function fmtMinutes(min: number) {
  return min < 90 ? `${Math.round(min)} min` : `${(min / 60).toFixed(1)} h`;
}

function fmtDays(days: number) {
  return days > 60 ? `${Math.round(days / 7)} weeks` : `${days.toFixed(days < 10 ? 1 : 0)} days`;
}

function BatteryChart({ pts }: { pts: { t: number; mv: number }[] }) {
  if (pts.length < 2) return <p class="muted small">The battery graph appears after a few check-ins.</p>;
  const t0 = pts[0].t, t1 = pts[pts.length - 1].t;
  const lo = Math.min(...pts.map((p) => p.mv), 3500), hi = Math.max(...pts.map((p) => p.mv), 4200);
  const x = (t: number) => ((t - t0) / Math.max(1, t1 - t0)) * 300;
  const y = (v: number) => 80 - ((v - lo) / Math.max(1, hi - lo)) * 70;
  const d = pts.map((p, i) => `${i ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.mv).toFixed(1)}`).join(' ');
  return (
    <svg class="batt-chart" viewBox="0 0 300 90" preserveAspectRatio="none" role="img"
      aria-label={`Battery from ${(pts[0].mv / 1000).toFixed(2)} V to ${(pts[pts.length - 1].mv / 1000).toFixed(2)} V`}>
      <path d={d} fill="none" stroke="var(--ok)" stroke-width="2" vector-effect="non-scaling-stroke" />
      <text x="2" y="88" font-size="9" fill="var(--ink-dim)">{new Date(t0).toLocaleString([], { weekday: 'short', hour: 'numeric' })}</text>
      <text x="298" y="88" font-size="9" fill="var(--ink-dim)" text-anchor="end">now</text>
    </svg>
  );
}

export function CollarView() {
  const points = useStore((s) => s.points);
  const config = useStore((s) => s.config);
  const queue = useStore((s) => s.queue);
  const base = useStore((s) => s.base);
  const collar = useStore((s) => s.collar);
  const live = useStore((s) => s.collarLive);
  const [draft, setDraft] = useState<CollarConfig>(config?.value ?? DEFAULT_CONFIG);
  const [sleepMin, setSleepMin] = useState(30);
  const direct = collar.status === 'connected';
  const viaBase = !direct && base.status === 'connected';
  const canSend = direct || viaBase;

  useEffect(() => { if (config) setDraft(config.value); }, [config?.t]);

  const last = points[points.length - 1];
  const lastFix = [...points].reverse().find((p) => p.fix);
  const battPts = useMemo(() => points.filter((p) => p.t > Date.now() - 48 * 3600_000 && p.batt_mv > 2500)
                                      .map((p) => ({ t: p.t, mv: p.batt_mv })), [points]);
  const avgGps = useMemo(() => {
    const r = points.slice(-20).filter((p) => p.gps_s > 0);
    return r.length ? r.reduce((a, p) => a + p.gps_s, 0) / r.length : 8;
  }, [points]);
  const battMv = live?.batt_mv ?? last?.batt_mv;
  const onUsb = live?.usb ?? (last ? (last.flags & FLAGS.USB) !== 0 : false);
  const findOn = live?.find ?? (last ? (last.flags & FLAGS.FIND) !== 0 : false);
  const changed = (k: keyof CollarConfig) => config && draft[k] !== config.value[k];
  const anyChanged = config ? FIELDS.some((f) => changed(f.key)) || draft.leds !== config.value.leds : true;
  const days = estimateDays(draft, avgGps);
  const trend = useMemo(() => daysLeftFromTrend(points), [points]);

  return (
    <div class="panel"><div class="panel-inner">
      <h2 class="section">{collar.name ?? 'Collar'}</h2>
      {direct ? (
        <div class="via"><IconBluetooth /><div>Connected to the collar directly. Changes are saved on it right away.</div></div>
      ) : viaBase ? (
        <div class="via"><IconRadio /><div>Talking to the collar through <b>{base.name}</b> over LoRa. Commands go out right after its next
          check-in{last ? `, expected within ${fmtInterval(last.interval_s)}` : ''}.</div></div>
      ) : (
        <div class="via"><IconRadio /><div>Connect to the base station, or to the collar over Bluetooth, to send commands.</div></div>
      )}

      <div class="stats">
        <div class="stat"><div class={`v ${battMv && battMv < (config?.value.low_batt_mv ?? 3500) && !onUsb ? 'warn' : ''}`}>
          {battMv ? `${battPercent(battMv)}%` : '?'}</div>
          <div class="k">{battMv ? `${(battMv / 1000).toFixed(2)} V${onUsb ? ', on USB' : ''}` : 'battery'}</div></div>
        <div class="stat"><div class="v">{onUsb ? 'Charging' : trend.days !== undefined ? fmtDays(trend.days)
            : trend.n >= TREND_CHECKINS && trend.minutes >= TREND_MIN_MINUTES ? 'Steady' : 'Measuring'}</div>
          <div class="k">{onUsb ? 'on USB power'
            : trend.days !== undefined ? `left until ${(BATT_CUTOFF_MV / 1000).toFixed(1)} V, measured over ${fmtMinutes(trend.minutes)}`
            : trend.n >= TREND_CHECKINS && trend.minutes >= TREND_MIN_MINUTES ? 'no measurable drain yet'
            : `battery time left needs ${TREND_CHECKINS} check-ins over ${TREND_MIN_MINUTES} min at one interval (${trend.n} so far, ${Math.round(trend.minutes)} min)`}</div></div>
        <div class="stat"><div class="v">{last ? ago(last.t).replace(' ago', '') : '?'}</div><div class="k">since last check-in</div></div>
        <div class="stat"><div class="v">{lastFix ? `±${lastFix.hacc_m} m` : '?'}</div>
          <div class="k">{lastFix ? `${lastFix.sats} satellites, ${lastFix.gps_s}s to fix` : 'no GPS fix yet'}</div></div>
        <div class="stat"><div class="v">{last?.rssi !== undefined ? `${Math.round(last.rssi)}` : '?'}</div>
          <div class="k">{last?.snr !== undefined ? `dBm signal, SNR ${last.snr.toFixed(1)}` : 'signal at base'}</div></div>
        <div class="stat"><div class="v">{findOn ? 'Find' : last ? fmtInterval(last.interval_s) : '?'}</div>
          <div class="k">{findOn ? 'fast reporting is on' : 'between check-ins'}</div></div>
      </div>

      <h2 class="section">Actions</h2>
      <div class="quick">
        <button class="btn cat" disabled={!canSend} onClick={() => collarAction('find', 10)}>Find mode, 10 min</button>
        <button class="btn" disabled={!canSend} onClick={() => collarAction('now')}>Locate now</button>
        <span style="display:inline-flex; gap:6px; align-items:center">
          <button class="btn" disabled={!canSend} onClick={() => collarAction('sleep', sleepMin)}>Go quiet for</button>
          <select aria-label="Quiet time" value={sleepMin} onChange={(e) => setSleepMin(+(e.target as HTMLSelectElement).value)}
            class="btn" style="padding: 8px 10px">
            {[15, 30, 60, 120, 240, 480].map((m) => <option value={m}>{m < 60 ? `${m} min` : `${m / 60} h`}</option>)}
          </select>
        </span>
      </div>
      <p class="muted small">Going quiet stops reports entirely; the collar can't be reached until it wakes up again.</p>

      {viaBase && queue.length > 0 && (
        <>
          <h2 class="section">Waiting to send</h2>
          {queue.map((q) => (
            <div class="queue-item">
              <span>{describeAction(q.action, q.arg)}</span>
              <span class="state">{q.tries ? `tried ${q.tries}×, retrying at next check-in` : 'after next check-in'}</span>
              <button class="btn quiet danger" onClick={() => cancelQueued(q.qid)}>Cancel</button>
            </div>
          ))}
        </>
      )}

      <h2 class="section">Battery, last 48 hours</h2>
      <BatteryChart pts={battPts} />

      <h2 class="section">Settings</h2>
      <p class="muted small" style="margin-top:0">
        {config ? `Last read from the collar ${ago(config.t)}${config.source === 'base' ? ' (via base)' : ''}.` : 'Not read from the collar yet; showing defaults.'}
        {' '}<button class="btn quiet" disabled={!canSend} onClick={() => collarAction('get_config')}>Read from collar</button>
      </p>
      {FIELDS.map((f) => (
        <div class="field">
          <label for={`f-${f.key}`}>{f.label}</label>
          <span style="display:inline-flex; align-items:center; gap:6px">
            <input id={`f-${f.key}`} type="number" class={changed(f.key) ? 'changed' : ''} min={f.min} max={f.max} step={f.step}
              value={f.toUi(draft[f.key] as number)}
              onInput={(e) => {
                const v = parseFloat((e.target as HTMLInputElement).value);
                if (!Number.isNaN(v)) setDraft({ ...draft, [f.key]: f.fromUi(v) });
              }} />
            <span class="muted small" style="min-width:30px">{f.unit}</span>
          </span>
          <div class="help">{f.help}</div>
        </div>
      ))}
      <div class="field">
        <label id="leds-label">LED blinks</label>
        <button class="switch" role="switch" aria-labelledby="leds-label" aria-checked={draft.leds}
          onClick={() => setDraft({ ...draft, leds: !draft.leds })} />
        <div class="help">Blue while searching for GPS, green when a report goes out. Off saves a little battery and keeps the collar dark at night.</div>
      </div>
      <p class="small" style="margin: 14px 0 0">
        Predicted battery life with the settings above: <b>{fmtDays(days)}</b>
        <span class="muted"> ({BATTERY_MAH} mAh battery, average GPS time {avgGps.toFixed(0)} s; a rough guide from a power
          model, not the measured battery trend shown at the top)</span>
      </p>
      {direct && (
        <>
          <h2 class="section">Firmware</h2>
          <p class="muted small" style="margin-top:0">{collar.fw ?? 'Unknown version'}.
            {live?.reset ? ` Last restart: ${live.reset}${live.uptime_s !== undefined ? `, ${fmtMinutes(live.uptime_s / 60)} ago` : ''}.` : ''} To update, restart the collar into update mode,
            then copy the new <b>cat_collar.uf2</b> onto the XIAO drive that appears on your computer.</p>
          <button class="btn" onClick={() => { if (confirm('Restart the collar into update mode? It stops tracking until new firmware is copied on or it is reset.')) void collarUpdateMode(); }}>
            Restart into update mode</button>
        </>
      )}
      {anyChanged && (
        <div class="sticky-save">
          <button class="btn primary" disabled={!canSend} onClick={() => sendConfig(draft)}>
            {direct ? 'Save to collar' : 'Send to collar'}
          </button>
          {config && <button class="btn quiet" onClick={() => setDraft(config.value)}>Undo changes</button>}
        </div>
      )}
    </div></div>
  );
}
