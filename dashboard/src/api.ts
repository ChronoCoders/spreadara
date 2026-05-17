// REST + WS client. Reconnect on disconnect with exponential backoff capped at 5s.

const API_BASE = (import.meta as any).env?.VITE_API_BASE || 'http://localhost:8080';
const WS_BASE = (import.meta as any).env?.VITE_WS_BASE || 'ws://localhost:8080';

export interface Snapshot {
  ts_ns: number;
  inventory: number;
  avg_entry: number;
  realized_pnl: number;
  unrealized_pnl: number;
  total_fees: number;
  mid_price: number;
  cum_total: number;
  gamma?: number;
  k?: number;
  t?: number;
}

export interface Trade {
  ts_ns: number;
  order_id: string;
  side: number;
  price: number;
  qty: number;
  fee: number;
  fee_asset: string;
}

export interface DailyPnl {
  date: string;
  realized: number;
  unrealized: number;
  fees: number;
  total: number;
}

export interface SystemEvent {
  ts_ns: number;
  severity: string;
  source: string;
  code: string;
  msg: string;
}

async function getJSON<T>(path: string): Promise<T> {
  const r = await fetch(`${API_BASE}${path}`);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

export const api = {
  snapshot: () => getJSON<Snapshot>('/api/snapshot'),
  trades: (limit = 100) => getJSON<Trade[]>(`/api/trades?limit=${limit}`),
  daily: () => getJSON<DailyPnl[]>('/api/pnl/daily'),
  events: (limit = 100) => getJSON<SystemEvent[]>(`/api/events?limit=${limit}`),
};

export type WsState = 'connected' | 'disconnected';

export class WebSocketClient {
  private ws: WebSocket | null = null;
  private backoff = 250;
  private closed = false;
  public state: WsState = 'disconnected';
  constructor(
    private onMessage: (s: Snapshot) => void,
    private onState?: (s: WsState) => void,
  ) {}

  connect() {
    this.closed = false;
    this.open();
  }

  close() {
    this.closed = true;
    if (!this.ws) return;
    // WHY: closing a CONNECTING socket logs a noisy "closed before connection
    // established" in the browser console. Detach handlers, then close once
    // the socket either opens or fails naturally. React 18 StrictMode unmounts
    // and re-mounts effects in dev, so a freshly-opened WS gets closed before
    // the handshake completes on every page load — this avoids the noise.
    const ws = this.ws;
    ws.onmessage = null;
    ws.onclose = null;
    ws.onerror = null;
    if (ws.readyState === WebSocket.CONNECTING) {
      ws.onopen = () => ws.close();
    } else {
      ws.close();
    }
  }

  private setState(s: WsState) {
    this.state = s;
    if (this.onState) this.onState(s);
  }

  private open() {
    try {
      this.ws = new WebSocket(`${WS_BASE}/ws`);
    } catch {
      this.setState('disconnected');
      this.scheduleReconnect();
      return;
    }
    this.ws.onopen = () => { this.backoff = 250; this.setState('connected'); };
    this.ws.onmessage = (e) => {
      try { this.onMessage(JSON.parse(e.data)); } catch { /* ignore */ }
    };
    this.ws.onclose = () => { this.setState('disconnected'); this.scheduleReconnect(); };
    this.ws.onerror = () => { /* onclose will handle */ };
  }

  private scheduleReconnect() {
    if (this.closed) return;
    const d = this.backoff;
    this.backoff = Math.min(this.backoff * 2, 5000);
    setTimeout(() => this.open(), d);
  }
}
