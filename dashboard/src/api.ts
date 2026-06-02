// REST + WS client. Reconnect on disconnect with exponential backoff capped at 5s.

// Use ?? (not ||) so an explicitly-empty VITE_API_BASE/VITE_WS_BASE means
// "same origin" (relative paths) in production rather than silently falling
// back to a localhost dev URL.
const API_BASE = (import.meta as any).env?.VITE_API_BASE ?? 'http://localhost:8080';
const WS_BASE = (import.meta as any).env?.VITE_WS_BASE ?? 'ws://localhost:8080';

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
  is_maker?: boolean;
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

export interface SpreadPoint {
  ts_ns: number;
  spread_bps: number;
}

export interface InventoryPoint {
  ts_ns: number;
  inventory: number;
  mid_price: number;
}

export interface OrdersPayload {
  open_count: number;
  events: SystemEvent[];
}

export interface FundingRate {
  funding_rate: number;
  next_funding_time: number;
  funding_rate_8h: number;
}

export interface BacktestRow {
  run_ts: string;
  total_pnl: number;
  sharpe_ratio: number;
  max_drawdown_pct: number;
  fill_count: number;
  maker_ratio: number;
  avg_spread_captured_bps: number;
  initial_capital: number;
  final_equity: number;
}

export interface CalibrationRow {
  gamma: number;
  k: number;
  t: number;
  sharpe: number;
  pnl: number;
  max_dd: number;
  fills: number;
}

export interface AlertRule {
  id: string;
  name: string;
  type: 'drawdown' | 'inventory' | 'pnl' | 'circuit_breaker';
  threshold: number;
  channel: 'webhook' | 'log';
  webhook_url?: string;
  enabled: boolean;
}

export interface LogsResponse {
  lines: string[];
  total_lines: number;
}

export interface SystemStatus {
  exchange: string;
  ws_streams: Record<string, string>;
  uptime_seconds: number;
  last_event_ts_ns: number;
  halted: boolean;
  ws_connected: boolean;
  pg_connected: boolean;
  last_snapshot_age_ms: number;
}


async function getJSON<T>(path: string): Promise<T> {
  const r = await fetch(`${API_BASE}${path}`);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

async function postJSON<T>(path: string, body?: unknown): Promise<T> {
  const r = await fetch(`${API_BASE}${path}`, {
    method: 'POST',
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

async function postText(path: string, body: string): Promise<{ status: string }> {
  const r = await fetch(`${API_BASE}${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body,
  });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

async function getText(path: string): Promise<string> {
  const r = await fetch(`${API_BASE}${path}`);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.text();
}

async function del(path: string): Promise<void> {
  const r = await fetch(`${API_BASE}${path}`, { method: 'DELETE' });
  if (!r.ok && r.status !== 204) throw new Error(`${r.status} ${r.statusText}`);
}

export const api = {
  snapshot: () => getJSON<Snapshot>('/api/snapshot'),
  trades: (limit = 100) => getJSON<Trade[]>(`/api/trades?limit=${limit}`),
  daily: () => getJSON<DailyPnl[]>('/api/pnl/daily'),
  events: (limit = 100) => getJSON<SystemEvent[]>(`/api/events?limit=${limit}`),
  spreads: (limit = 1000) => getJSON<SpreadPoint[]>(`/api/spreads?limit=${limit}`),
  inventory: (limit = 1000) => getJSON<InventoryPoint[]>(`/api/inventory?limit=${limit}`),
  orders: (limit = 50) => getJSON<OrdersPayload>(`/api/orders?limit=${limit}`),
  fundingRate: () => getJSON<FundingRate>('/api/funding-rate'),
  calibration: () => getJSON<CalibrationRow[]>('/api/calibration'),
  calibrationRun: () => postJSON<{ status: string }>('/api/calibration/run'),
  backtest: () => getJSON<BacktestRow[]>('/api/backtest'),
  backtestRun: () => postJSON<{ status: string }>('/api/backtest/run'),
  status: () => getJSON<SystemStatus>('/api/v5/status'),
  alerts: () => getJSON<AlertRule[]>('/api/alerts'),
  alertUpsert: (rule: AlertRule) => postJSON<AlertRule>('/api/alerts', rule),
  alertDelete: (id: string) => del(`/api/alerts?id=${encodeURIComponent(id)}`),
  logs: (lines = 200) => getJSON<LogsResponse>(`/api/logs?lines=${lines}`),
  configGet: () => getText('/api/config'),
  configSave: (content: string) => postText('/api/config', content),
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
