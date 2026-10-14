import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import { collarAction, updateSettings, useStore, watchMe } from '../store';
import { ago, bearingDeg, compass, distanceM, fmtDistance } from '../geo';
import { IconLayers, IconMe, IconTarget } from '../icons';
import type { ReportPoint } from '../types';

const EYES_SVG = (cls: string) => `
<div class="cat-eyes ${cls}"><svg width="44" height="44" viewBox="0 0 44 44">
<circle cx="22" cy="22" r="20" fill="#0f1c2e" stroke="#f2b33d" stroke-width="2"/>
<g class="eyes"><path d="M9.5 21c2-4 8-4 10 0-2 4-8 4-10 0z" fill="#f2b33d"/><path d="M24.5 21c2-4 8-4 10 0-2 4-8 4-10 0z" fill="#f2b33d"/>
<path d="M14.5 18v6M29.5 18v6" stroke="#0f1c2e" stroke-width="2.2" stroke-linecap="round"/></g></svg></div>`;

const TRAILS: { label: string; hours: number }[] = [
  { label: '1 h', hours: 1 }, { label: '6 h', hours: 6 }, { label: '24 h', hours: 24 }, { label: 'All', hours: 0 },
];

export function MapView() {
  const el = useRef<HTMLDivElement>(null);
  const map = useRef<L.Map>();
  const layers = useRef<{ street: L.TileLayer; satellite: L.TileLayer; trail: L.LayerGroup; cat?: L.Marker; acc?: L.Circle; me?: L.Marker; meAcc?: L.Circle }>();
  const points = useStore((s) => s.points);
  const settings = useStore((s) => s.settings);
  const me = useStore((s) => s.me);
  const baseConnected = useStore((s) => s.base.status === 'connected' || s.collar.status === 'connected');
  const [, tick] = useState(0);

  const located = useMemo(() => points.filter((p) => p.lat !== undefined && p.lon !== undefined), [points]);
  const last: ReportPoint | undefined = located[located.length - 1];
  const lastAny: ReportPoint | undefined = points[points.length - 1];

  // refresh "x min ago" text
  useEffect(() => { const t = setInterval(() => tick((n) => n + 1), 15000); return () => clearInterval(t); }, []);
  useEffect(() => { watchMe(settings.showMe); }, [settings.showMe]);

  // create the map once
  useEffect(() => {
    if (!el.current || map.current) return;
    const m = L.map(el.current, { zoomControl: false, attributionControl: true });
    const street = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19, attribution: '&copy; OpenStreetMap contributors' });
    const satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
      maxZoom: 19, attribution: 'Imagery &copy; Esri' });
    (settings.layer === 'satellite' ? satellite : street).addTo(m);
    const trail = L.layerGroup().addTo(m);
    m.on('dragstart', () => updateSettings({ followCat: false }));
    layers.current = { street, satellite, trail };
    map.current = m;
    if (last) m.setView([last.lat!, last.lon!], 17);
    else m.setView([39.5, -98.35], 4);
    const ro = new ResizeObserver(() => m.invalidateSize());
    ro.observe(el.current);
    return () => { ro.disconnect(); m.remove(); map.current = undefined; };
  }, []);

  // base layer
  useEffect(() => {
    const m = map.current, ly = layers.current;
    if (!m || !ly) return;
    const on = settings.layer === 'satellite' ? ly.satellite : ly.street;
    const off = settings.layer === 'satellite' ? ly.street : ly.satellite;
    if (m.hasLayer(off)) m.removeLayer(off);
    if (!m.hasLayer(on)) on.addTo(m);
  }, [settings.layer]);

  // trail + cat marker
  useEffect(() => {
    const m = map.current, ly = layers.current;
    if (!m || !ly) return;
    ly.trail.clearLayers();
    const since = settings.trailHours ? Date.now() - settings.trailHours * 3600_000 : 0;
    const pts = located.filter((p) => p.t >= since);
    for (let i = 1; i < pts.length; i++) {
      const a = pts[i - 1], b = pts[i];
      const age = (i / pts.length);
      L.polyline([[a.lat!, a.lon!], [b.lat!, b.lon!]], { color: '#f2b33d', weight: 4, opacity: 0.15 + 0.75 * age, interactive: false }).addTo(ly.trail);
    }
    for (const p of pts.slice(0, -1)) {
      L.circleMarker([p.lat!, p.lon!], { radius: 3, color: '#f2b33d', weight: 1, fillOpacity: p.fix ? 0.8 : 0.2, opacity: 0.6 })
        .bindTooltip(`${new Date(p.t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}${p.fix ? '' : ' (last known)'}`)
        .addTo(ly.trail);
    }
    if (last) {
      const ll: L.LatLngExpression = [last.lat!, last.lon!];
      const fresh = Date.now() - last.t < Math.max(2 * last.interval_s * 1000, 120_000) && last.fix;
      const icon = L.divIcon({ html: EYES_SVG(fresh ? 'fresh' : 'old'), className: '', iconSize: [44, 44], iconAnchor: [22, 22] });
      if (!ly.cat) ly.cat = L.marker(ll, { icon, zIndexOffset: 1000, keyboard: false }).addTo(m);
      else { ly.cat.setLatLng(ll); ly.cat.setIcon(icon); }
      if (!ly.acc) ly.acc = L.circle(ll, { radius: last.hacc_m, color: '#f2b33d', weight: 1, fillOpacity: 0.08, interactive: false }).addTo(m);
      else { ly.acc.setLatLng(ll); ly.acc.setRadius(Math.min(last.hacc_m, 5000)); }
      if (settings.followCat) m.panTo(ll, { animate: true });
    }
  }, [located, settings.trailHours]);

  // my position
  useEffect(() => {
    const m = map.current, ly = layers.current;
    if (!m || !ly) return;
    if (!me) { ly.me?.remove(); ly.meAcc?.remove(); ly.me = ly.meAcc = undefined; return; }
    const ll: L.LatLngExpression = [me.lat, me.lon];
    if (!ly.me) {
      ly.me = L.marker(ll, { icon: L.divIcon({ html: '<div class="me-dot"></div>', className: '', iconSize: [16, 16], iconAnchor: [8, 8] }), keyboard: false }).addTo(m);
      ly.meAcc = L.circle(ll, { radius: me.acc, color: '#5fb7ff', weight: 1, fillOpacity: 0.08, interactive: false }).addTo(m);
      if (!last) m.setView(ll, 16);
    } else { ly.me.setLatLng(ll); ly.meAcc!.setLatLng(ll); ly.meAcc!.setRadius(me.acc); }
  }, [me]);

  const centerCat = () => { if (last && map.current) { map.current.setView([last.lat!, last.lon!], Math.max(map.current.getZoom(), 17)); updateSettings({ followCat: true }); } };
  const centerMe = () => {
    if (!settings.showMe) updateSettings({ showMe: true });
    if (me && map.current) { map.current.setView([me.lat, me.lon], Math.max(map.current.getZoom(), 16)); updateSettings({ followCat: false }); }
  };
  const fitBoth = () => {
    if (me && last && map.current) map.current.fitBounds(L.latLngBounds([[me.lat, me.lon], [last.lat!, last.lon!]]).pad(0.3));
  };

  const dist = me && last ? distanceM(me, { lat: last.lat!, lon: last.lon! }) : undefined;
  const brg = me && last ? bearingDeg(me, { lat: last.lat!, lon: last.lon! }) : undefined;
  const stale = last ? Date.now() - last.t > Math.max(3 * last.interval_s * 1000, 15 * 60_000) : false;

  return (
    <div class="mapwrap">
      <div class="map" ref={el} />
      <div class="trail-pick" role="group" aria-label="Trail length">
        {TRAILS.map((t) => (
          <button aria-pressed={settings.trailHours === t.hours} onClick={() => updateSettings({ trailHours: t.hours })}>{t.label}</button>
        ))}
      </div>
      <div class="map-tools">
        <button class="btn" title="Center on cat" aria-label="Center on cat" onClick={centerCat}><IconTarget /></button>
        <button class="btn" title="Center on me" aria-label="Center on me" onClick={centerMe}><IconMe /></button>
        <button class="btn" title="Switch map style" aria-label="Switch between street and satellite map"
          onClick={() => updateSettings({ layer: settings.layer === 'street' ? 'satellite' : 'street' })}><IconLayers /></button>
      </div>

      {last ? (
        <div class="readout">
          <svg class="dial" viewBox="0 0 64 64" aria-hidden="true" onClick={fitBoth}>
            <circle cx="32" cy="32" r="29" fill="none" stroke="currentColor" stroke-opacity="0.25" stroke-width="2" />
            <text x="32" y="12" text-anchor="middle" font-size="9" fill="currentColor" opacity="0.7">N</text>
            {brg !== undefined && (
              <g transform={`rotate(${brg} 32 32)`}>
                <path d="M32 10 40 36 32 31 24 36z" fill="#f2b33d" />
              </g>
            )}
            {brg === undefined && <circle cx="32" cy="32" r="6" fill="#f2b33d" />}
          </svg>
          <div class="big">
            {dist !== undefined ? <>{fmtDistance(dist)}<small>{compass(brg!)}</small></> : <>Last seen<small>{ago(last.t)}</small></>}
          </div>
          <div class="meta">
            {dist !== undefined && <span class={stale ? 'stale' : ''}>{ago(last.t)}</span>}
            <span>±{last.hacc_m < 65535 ? `${last.hacc_m} m` : '?'}</span>
            {lastAny && !lastAny.fix && <span class="stale">no fix on last check-in</span>}
            {last.rssi !== undefined && <span>{Math.round(last.rssi)} dBm</span>}
          </div>
          <div class="actions">
            <button class="btn cat" disabled={!baseConnected} onClick={() => collarAction('find', 10)}>Find mode, 10 min</button>
            <button class="btn" disabled={!baseConnected} onClick={() => collarAction('now')}>Locate now</button>
            <a class="btn quiet" href={`https://www.google.com/maps/dir/?api=1&destination=${last.lat},${last.lon}&travelmode=walking`}
               target="_blank" rel="noopener">Walk there in Google Maps</a>
          </div>
        </div>
      ) : (
        <div class="readout empty">
          <div>
            <div class="big" style="color: var(--ink); font-size: 22px">No position yet</div>
            <div class="meta">{baseConnected ? 'Waiting for the collar to check in. The first GPS fix can take a couple of minutes outdoors.'
                                             : 'Connect to your base station to see the collar on the map.'}</div>
          </div>
        </div>
      )}
    </div>
  );
}
