import { useEffect, useState } from 'preact/hooks';
import { bleSupported, serialSupported, type Role } from './link';
import { connect, disconnect, useStore, type Conn } from './store';
import { MapView } from './components/MapView';
import { CollarView } from './components/CollarView';
import { ChatView } from './components/ChatView';
import { LogView } from './components/LogView';
import { CatEyes, IconBluetooth, IconChat, IconList, IconMap, IconPlay, IconTag, IconUsb } from './icons';

type Tab = 'map' | 'collar' | 'chat' | 'log';
const TABS: { id: Tab; label: string; icon: () => preact.JSX.Element }[] = [
  { id: 'map', label: 'Map', icon: IconMap },
  { id: 'collar', label: 'Collar', icon: IconTag },
  { id: 'chat', label: 'Chat', icon: IconChat },
  { id: 'log', label: 'Activity', icon: IconList },
];

function ConnSection({ role, conn }: { role: Role; conn: Conn }) {
  const title = role === 'base' ? 'Base station' : 'Collar, up close';
  const blurb = role === 'base'
    ? 'The Heltec at home. It hears the collar over LoRa and passes everything to this app.'
    : 'Talk to the collar directly. It is visible for a few minutes after power-up or a press of its button.';
  return (
    <div>
      <h3>{title}</h3>
      {conn.status === 'connected' ? (
        <>
          <p>Connected to <b>{conn.name}</b>{conn.fw ? `, ${conn.fw}` : ''}.</p>
          <div class="row"><button class="btn danger" onClick={() => disconnect(role)}>Disconnect</button></div>
        </>
      ) : (
        <>
          <p>{blurb}{conn.error ? <><br /><span style="color:var(--danger)">{conn.error}</span></> : null}</p>
          <div class="row">
            <button class="btn primary" disabled={!bleSupported() || conn.status === 'connecting'} onClick={() => connect(role, 'ble')}>
              <IconBluetooth />Bluetooth</button>
            <button class="btn" disabled={!serialSupported() || conn.status === 'connecting'} onClick={() => connect(role, 'serial')}>
              <IconUsb />USB</button>
            <button class="btn quiet" onClick={() => connect(role, 'demo')}><IconPlay />Demo</button>
          </div>
        </>
      )}
    </div>
  );
}

export function App() {
  const [tab, setTab] = useState<Tab>('map');
  const [menu, setMenu] = useState(false);
  const base = useStore((s) => s.base);
  const collar = useStore((s) => s.collar);
  const toast = useStore((s) => s.toast);
  const syncing = useStore((s) => s.syncing);
  const chatCount = useStore((s) => s.chat.length);
  const [seenChat, setSeenChat] = useState(chatCount);
  const [wide, setWide] = useState(() => window.matchMedia('(min-width: 900px)').matches);

  useEffect(() => {
    const mq = window.matchMedia('(min-width: 900px)');
    const f = () => setWide(mq.matches);
    mq.addEventListener('change', f);
    return () => mq.removeEventListener('change', f);
  }, []);
  useEffect(() => { if (wide && tab === 'map') setTab('collar'); }, [wide]);
  useEffect(() => { if (tab === 'chat') setSeenChat(chatCount); }, [tab, chatCount]);
  useEffect(() => { if (base.status === 'connected' || collar.status === 'connected') setMenu(false); }, [base.status, collar.status]);

  const unread = Math.max(0, chatCount - seenChat);
  const noLinks = !bleSupported() && !serialSupported();

  const tabBar = (
    <nav class="tabs" role="tablist">
      {TABS.map((t) => (
        <button class="tab" role="tab" data-tab={t.id} aria-selected={tab === t.id} onClick={() => setTab(t.id)}>
          <t.icon />{t.label}
          {t.id === 'chat' && unread > 0 && tab !== 'chat' && <span class="badge">{unread}</span>}
        </button>
      ))}
    </nav>
  );

  const content = tab === 'collar' ? <CollarView /> : tab === 'chat' ? <ChatView /> : tab === 'log' ? <LogView /> : null;

  return (
    <div class="shell">
      <header class="topbar">
        <div class="brand"><CatEyes size={26} />Cat Tracker</div>
        <button class="chip" onClick={() => setMenu(!menu)} aria-expanded={menu} aria-label="Connections">
          <span class={`dot ${base.status}`} />
          {base.status === 'connected' ? (syncing ? 'Syncing…' : base.name) : base.status === 'connecting' ? 'Connecting…' : 'Connect'}
        </button>
        {collar.status !== 'disconnected' && (
          <button class="chip" onClick={() => setMenu(!menu)} aria-label="Collar connection">
            <span class={`dot ${collar.status}`} />{collar.name ?? 'Collar'}
          </button>
        )}
        {menu && (
          <div class="menu">
            {noLinks && <p style="color:var(--danger)">This browser can't use Bluetooth or USB devices. Open the app in Chrome or Edge on Android, Windows or Mac.</p>}
            <ConnSection role="base" conn={base} />
            <ConnSection role="collar" conn={collar} />
          </div>
        )}
      </header>

      <div class="main">
        {(wide || tab === 'map') && <MapView />}
        {wide ? <div class="side">{tabBar}{content}</div> : tab !== 'map' && content}
      </div>
      {!wide && tabBar}

      {toast && <div class={`toast ${toast.kind}`} role="status">{toast.text}</div>}
    </div>
  );
}
