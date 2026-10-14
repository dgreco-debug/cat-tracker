// Small geometry and formatting helpers.

export function distanceM(a: { lat: number; lon: number }, b: { lat: number; lon: number }) {
  const R = 6371000, toR = Math.PI / 180;
  const dLat = (b.lat - a.lat) * toR, dLon = (b.lon - a.lon) * toR;
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * toR) * Math.cos(b.lat * toR) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(s));
}

export function bearingDeg(a: { lat: number; lon: number }, b: { lat: number; lon: number }) {
  const toR = Math.PI / 180;
  const y = Math.sin((b.lon - a.lon) * toR) * Math.cos(b.lat * toR);
  const x = Math.cos(a.lat * toR) * Math.sin(b.lat * toR) - Math.sin(a.lat * toR) * Math.cos(b.lat * toR) * Math.cos((b.lon - a.lon) * toR);
  return (Math.atan2(y, x) / toR + 360) % 360;
}

export function compass(deg: number) {
  return ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(deg / 45) % 8];
}

export function fmtDistance(m: number, imperial = true) {
  if (imperial) {
    const ft = m * 3.28084;
    return ft < 1000 ? `${Math.round(ft)} ft` : `${(m / 1609.34).toFixed(ft < 5280 * 10 ? 2 : 1)} mi`;
  }
  return m < 1000 ? `${Math.round(m)} m` : `${(m / 1000).toFixed(2)} km`;
}

export function ago(t: number, now = Date.now()) {
  const s = Math.max(0, Math.round((now - t) / 1000));
  if (s < 5) return 'just now';
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} h ${m % 60} min ago`;
  return new Date(t).toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' });
}

export function fmtInterval(s: number) {
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.round(s / 6) / 10} min`;
  return `${Math.round(s / 360) / 10} h`;
}

/** Collar temperature as "72 °F (22.0 °C)". */
export function fmtTemp(c: number) {
  return `${Math.round(c * 9 / 5 + 32)} °F (${c.toFixed(1)} °C)`;
}

/** Voltage at which the collar is treated as empty (the cell's protection cuts off around here). */
export const BATT_CUTOFF_MV = 3000;

/** Rough single-cell LiPo state of charge from resting voltage. */
export function battPercent(mv: number) {
  const curve: [number, number][] = [[BATT_CUTOFF_MV, 0], [3300, 2], [3500, 5], [3600, 12], [3700, 30], [3750, 42], [3800, 55], [3850, 65],
                                      [3900, 74], [3950, 82], [4000, 88], [4100, 96], [4200, 100]];
  if (mv <= curve[0][0]) return 0;
  for (let i = 1; i < curve.length; i++) {
    const [v1, p1] = curve[i - 1], [v2, p2] = curve[i];
    if (mv <= v2) return Math.round(p1 + ((mv - v1) / (v2 - v1)) * (p2 - p1));
  }
  return 100;
}

export function signalLabel(rssi?: number, snr?: number) {
  if (rssi === undefined) return '';
  const q = snr !== undefined && snr < -10 ? 'weak' : rssi > -90 ? 'strong' : rssi > -110 ? 'good' : 'weak';
  return q;
}
