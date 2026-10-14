import { useEffect, useRef, useState } from 'preact/hooks';
import { sendChat, setBaseName, useStore } from '../store';
import { ago } from '../geo';
import { IconSend } from '../icons';

const enc = new TextEncoder();

export function ChatView() {
  const chat = useStore((s) => s.chat);
  const nodes = useStore((s) => s.nodes);
  const base = useStore((s) => s.base);
  const max = useStore((s) => s.baseInfo.text_max ?? 112);
  const [text, setText] = useState('');
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState('');
  const list = useRef<HTMLDivElement>(null);
  const bytes = enc.encode(text).length;
  const connected = base.status === 'connected';

  useEffect(() => { list.current?.scrollTo({ top: list.current.scrollHeight }); }, [chat.length]);

  const submit = () => {
    const t = text.trim();
    if (!t || bytes > max || !connected) return;
    void sendChat(t);
    setText('');
  };

  const statusText = (s?: string, to?: string[]) =>
    s === 'sending' ? 'sending…' : s === 'sent' ? 'sent, waiting for a reply from another base' :
    s === 'delivered' ? `received by ${to?.join(', ')}` : s === 'unconfirmed' ? 'no other base confirmed it' : s === 'error' ? 'not sent' : '';

  const recent = nodes.filter((n) => Date.now() - n.lastSeen < 24 * 3600_000).sort((a, b) => b.lastSeen - a.lastSeen);

  return (
    <div class="chat">
      <div class="people">
        {connected && (renaming ? (
          <form style="display:flex; gap:6px" onSubmit={(e) => { e.preventDefault(); if (name.trim()) void setBaseName(name.trim().slice(0, 16)); setRenaming(false); }}>
            <input aria-label="Your name on the mesh" value={name} maxLength={16} onInput={(e) => setName((e.target as HTMLInputElement).value)}
              style="background:var(--night-2); border:1px solid var(--night-3); border-radius:999px; padding:4px 10px; width:150px" />
            <button class="btn" style="padding:4px 12px">Save</button>
          </form>
        ) : (
          <button class="person" style="border:0; cursor:pointer" onClick={() => { setName(base.name ?? ''); setRenaming(true); }}>
            <span>Your name: <b>{base.name}</b></span><span class="muted">Change</span>
          </button>
        ))}
        {recent.map((n) => (
          <span class="person" title={`Heard ${ago(n.lastSeen)}${n.rssi !== undefined ? `, ${Math.round(n.rssi)} dBm` : ''}`}>
            <span class={`dot ${Date.now() - n.lastSeen < 30 * 60_000 ? 'connected' : ''}`} />{n.name}
          </span>
        ))}
        {!recent.length && <span class="muted small" style="align-self:center">No other bases heard yet</span>}
      </div>

      <div class="chat-list" ref={list} aria-live="polite">
        {chat.length === 0 && (
          <div class="empty-state">
            <h3>Talk to the other base</h3>
            <p>Messages travel over LoRa to every base on your network, about {Math.round(max / 14)} radio packets at most, so keep them short.</p>
          </div>
        )}
        {chat.map((m) => (
          <div class={`bubble ${m.mine ? 'mine' : ''}`}>
            {!m.mine && <div class="who">{m.name}</div>}
            <div class="text">{m.text}</div>
            <div class="when">{new Date(m.t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}
              {m.mine && m.status ? `, ${statusText(m.status, m.deliveredTo)}` : ''}</div>
          </div>
        ))}
      </div>

      <form class="compose" onSubmit={(e) => { e.preventDefault(); submit(); }}>
        <textarea rows={1} placeholder={connected ? 'Message' : 'Connect to the base to chat'} disabled={!connected} value={text}
          aria-label="Message"
          onInput={(e) => setText((e.target as HTMLTextAreaElement).value)}
          onKeyDown={(e) => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); submit(); } }} />
        <span class={`count ${bytes > max ? 'over' : ''}`}>{bytes}/{max}</span>
        <button class="btn primary" aria-label="Send message" disabled={!connected || !text.trim() || bytes > max}><IconSend /></button>
      </form>
    </div>
  );
}
