// Transports to the base station and the collar. Both speak newline-delimited JSON over either
// Bluetooth LE (Nordic UART service) or USB serial, so the rest of the app does not care which.
import type { Incoming } from './types';

export const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // app -> device
const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // device -> app

export type Role = 'base' | 'collar';
export type LinkKind = 'ble' | 'serial' | 'demo';

export interface LinkHandlers {
  onMessage(m: Incoming): void;
  onRaw(line: string): void;
  onClose(reason?: string): void;
}

export interface Link {
  kind: LinkKind;
  label: string;
  send(obj: object): Promise<void>;
  close(): Promise<void>;
}

export const bleSupported = () => typeof navigator !== 'undefined' && 'bluetooth' in navigator;
export const serialSupported = () => typeof navigator !== 'undefined' && 'serial' in navigator;

/** Splits a byte/text stream into lines and hands JSON lines to the handler. */
class LineSplitter {
  private buf = '';
  constructor(private h: LinkHandlers) {}
  push(text: string) {
    this.buf += text;
    let i: number;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).replace(/\r$/, '');
      this.buf = this.buf.slice(i + 1);
      if (!line.trim()) continue;
      if (line.startsWith('{')) {
        try { this.h.onMessage(JSON.parse(line)); } catch { this.h.onRaw(line); }
      } else {
        this.h.onRaw(line);
      }
    }
    if (this.buf.length > 8192) this.buf = '';
  }
}

export async function connectBle(role: Role, h: LinkHandlers): Promise<Link> {
  const prefix = role === 'base' ? 'CatBase' : 'CatCollar';
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ namePrefix: prefix }, { services: [NUS_SERVICE] }],
    optionalServices: [NUS_SERVICE],
  });
  const server = await device.gatt!.connect();
  const svc = await server.getPrimaryService(NUS_SERVICE);
  const rx = await svc.getCharacteristic(NUS_RX);
  const tx = await svc.getCharacteristic(NUS_TX);
  const decoder = new TextDecoder();
  const split = new LineSplitter(h);
  tx.addEventListener('characteristicvaluechanged', (e) => {
    const v = (e.target as BluetoothRemoteGATTCharacteristic).value!;
    split.push(decoder.decode(v, { stream: true }));
  });
  await tx.startNotifications();
  let closed = false;
  device.addEventListener('gattserverdisconnected', () => { if (!closed) { closed = true; h.onClose('Bluetooth disconnected'); } });

  const enc = new TextEncoder();
  let chunk = 180;                      // shrinks to 20 if the link has a small MTU
  let chain: Promise<void> = Promise.resolve();
  const writeAll = async (bytes: Uint8Array) => {
    for (let off = 0; off < bytes.length;) {
      const part = bytes.slice(off, off + chunk);
      try {
        await rx.writeValueWithResponse(part);
        off += part.length;
      } catch (err) {
        if (chunk > 20) { chunk = 20; continue; }
        throw err;
      }
    }
  };
  return {
    kind: 'ble',
    label: device.name || prefix,
    send: (obj) => (chain = chain.then(() => writeAll(enc.encode(JSON.stringify(obj) + '\n')))),
    close: async () => { closed = true; try { await tx.stopNotifications(); } catch { /* ignore */ } device.gatt?.disconnect(); },
  };
}

export async function connectSerial(role: Role, h: LinkHandlers): Promise<Link> {
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: 115200 });
  const split = new LineSplitter(h);
  const decoder = new TextDecoder();
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  let closed = false;
  (async () => {
    try {
      while (port.readable && !closed) {
        reader = port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            if (value) split.push(decoder.decode(value, { stream: true }));
          }
        } finally { reader.releaseLock(); }
      }
    } catch { /* port lost */ }
    if (!closed) { closed = true; h.onClose('USB disconnected'); }
  })();
  const enc = new TextEncoder();
  let chain: Promise<void> = Promise.resolve();
  return {
    kind: 'serial',
    label: role === 'base' ? 'Base (USB)' : 'Collar (USB)',
    send: (obj) => (chain = chain.then(async () => {
      const w = port.writable!.getWriter();
      try { await w.write(enc.encode(JSON.stringify(obj) + '\n')); } finally { w.releaseLock(); }
    })),
    close: async () => {
      closed = true;
      try { await reader?.cancel(); } catch { /* ignore */ }
      try { await port.close(); } catch { /* ignore */ }
    },
  };
}
