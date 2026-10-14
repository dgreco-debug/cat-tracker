import { useState } from 'preact/hooks';
import { clearLog, clearTrail, useStore } from '../store';
import type { LogKind } from '../types';

const FILTERS: { label: string; kinds: LogKind[] | null }[] = [
  { label: 'Everything', kinds: null },
  { label: 'Check-ins', kinds: ['report'] },
  { label: 'Commands', kinds: ['cmd'] },
  { label: 'Chat', kinds: ['chat'] },
  { label: 'Device messages', kinds: ['device', 'error', 'info'] },
];

export function LogView() {
  const log = useStore((s) => s.log);
  const points = useStore((s) => s.points);
  const [f, setF] = useState(0);
  const kinds = FILTERS[f].kinds;
  const rows = log.filter((l) => !kinds || kinds.includes(l.kind)).slice(-300).reverse();

  const exportCsv = () => {
    const head = 'time,seq,fix,lat,lon,accuracy_m,sats,gps_s,battery_mv,interval_s,rssi,snr,temp_c\n';
    const body = points.map((p) => [new Date(p.t).toISOString(), p.seq, p.fix, p.lat ?? '', p.lon ?? '', p.hacc_m, p.sats, p.gps_s,
                                     p.batt_mv, p.interval_s, p.rssi ?? '', p.snr ?? '', p.temp_c ?? ''].join(',')).join('\n');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([head + body], { type: 'text/csv' }));
    a.download = `cat-track-${new Date().toISOString().slice(0, 10)}.csv`;
    a.click();
  };

  return (
    <div class="panel"><div class="panel-inner">
      <div class="log-filter" role="group" aria-label="Filter">
        {FILTERS.map((x, i) => <button aria-pressed={f === i} onClick={() => setF(i)}>{x.label}</button>)}
      </div>
      {rows.length === 0 && <div class="empty-state"><h3>Nothing here yet</h3><p>Check-ins, commands and messages show up as they happen.</p></div>}
      {rows.map((r) => (
        <div class={`log-row ${r.kind}`}>
          <time dateTime={new Date(r.t).toISOString()}>{new Date(r.t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}</time>
          <div class="txt">{r.text}</div>
        </div>
      ))}
      <h2 class="section">Stored data</h2>
      <p class="muted small">{points.length} check-ins are saved in this browser.</p>
      <div class="quick">
        <button class="btn" disabled={!points.length} onClick={exportCsv}>Download track as CSV</button>
        <button class="btn quiet danger" onClick={() => { if (confirm('Delete the saved track from this browser?')) clearTrail(); }}>Delete track</button>
        <button class="btn quiet danger" onClick={clearLog}>Clear log</button>
      </div>
    </div></div>
  );
}
