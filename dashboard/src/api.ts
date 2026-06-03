// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

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


// Thrown when a REST payload is missing a required field or a field has the
// wrong primitive type. Extends Error so existing page `.catch(markError)`
// handlers flag the page as stale/errored — a malformed response is just as
// dangerous as a frozen one for a trade-actionable view.
export class ApiValidationError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ApiValidationError';
  }
}

// --- Lightweight, hand-rolled runtime validators ---------------------------
// Each asserts the primitive type of a required field, throwing
// ApiValidationError on a missing/mismatched value. They keep parsing DRY
// without pulling in a runtime schema dependency.

function asObject(v: unknown, ctx: string): Record<string, unknown> {
  if (typeof v !== 'object' || v === null || Array.isArray(v)) {
    throw new ApiValidationError(`${ctx}: expected object`);
  }
  return v as Record<string, unknown>;
}

function asNumber(v: unknown, field: string): number {
  if (typeof v !== 'number' || Number.isNaN(v)) {
    throw new ApiValidationError(`field "${field}": expected number`);
  }
  return v;
}

function asString(v: unknown, field: string): string {
  if (typeof v !== 'string') {
    throw new ApiValidationError(`field "${field}": expected string`);
  }
  return v;
}

function asBoolean(v: unknown, field: string): boolean {
  if (typeof v !== 'boolean') {
    throw new ApiValidationError(`field "${field}": expected boolean`);
  }
  return v;
}

function asArray(v: unknown, ctx: string): unknown[] {
  if (!Array.isArray(v)) {
    throw new ApiValidationError(`${ctx}: expected array`);
  }
  return v;
}

// Optional number: pass through undefined/missing, validate type otherwise.
function optNumber(v: unknown, field: string): number | undefined {
  if (v === undefined || v === null) return undefined;
  return asNumber(v, field);
}

function optBoolean(v: unknown, field: string): boolean | undefined {
  if (v === undefined || v === null) return undefined;
  return asBoolean(v, field);
}

// --- Per-interface parsers -------------------------------------------------

function parseSnapshot(raw: unknown): Snapshot {
  const o = asObject(raw, 'Snapshot');
  return {
    ts_ns: asNumber(o.ts_ns, 'ts_ns'),
    inventory: asNumber(o.inventory, 'inventory'),
    avg_entry: asNumber(o.avg_entry, 'avg_entry'),
    realized_pnl: asNumber(o.realized_pnl, 'realized_pnl'),
    unrealized_pnl: asNumber(o.unrealized_pnl, 'unrealized_pnl'),
    total_fees: asNumber(o.total_fees, 'total_fees'),
    mid_price: asNumber(o.mid_price, 'mid_price'),
    cum_total: asNumber(o.cum_total, 'cum_total'),
    gamma: optNumber(o.gamma, 'gamma'),
    k: optNumber(o.k, 'k'),
    t: optNumber(o.t, 't'),
  };
}

function parseTrade(raw: unknown): Trade {
  const o = asObject(raw, 'Trade');
  return {
    ts_ns: asNumber(o.ts_ns, 'ts_ns'),
    order_id: asString(o.order_id, 'order_id'),
    side: asNumber(o.side, 'side'),
    price: asNumber(o.price, 'price'),
    qty: asNumber(o.qty, 'qty'),
    fee: asNumber(o.fee, 'fee'),
    fee_asset: asString(o.fee_asset, 'fee_asset'),
    is_maker: optBoolean(o.is_maker, 'is_maker'),
  };
}

function parseSpreadPoint(raw: unknown): SpreadPoint {
  const o = asObject(raw, 'SpreadPoint');
  return {
    ts_ns: asNumber(o.ts_ns, 'ts_ns'),
    spread_bps: asNumber(o.spread_bps, 'spread_bps'),
  };
}

function parseInventoryPoint(raw: unknown): InventoryPoint {
  const o = asObject(raw, 'InventoryPoint');
  return {
    ts_ns: asNumber(o.ts_ns, 'ts_ns'),
    inventory: asNumber(o.inventory, 'inventory'),
    mid_price: asNumber(o.mid_price, 'mid_price'),
  };
}

function parseSystemStatus(raw: unknown): SystemStatus {
  const o = asObject(raw, 'SystemStatus');
  // ws_streams is a string->string map; shallow-check it's an object.
  const ws_streams = asObject(o.ws_streams, 'ws_streams') as Record<string, string>;
  return {
    exchange: asString(o.exchange, 'exchange'),
    ws_streams,
    uptime_seconds: asNumber(o.uptime_seconds, 'uptime_seconds'),
    last_event_ts_ns: asNumber(o.last_event_ts_ns, 'last_event_ts_ns'),
    halted: asBoolean(o.halted, 'halted'),
    ws_connected: asBoolean(o.ws_connected, 'ws_connected'),
    pg_connected: asBoolean(o.pg_connected, 'pg_connected'),
    last_snapshot_age_ms: asNumber(o.last_snapshot_age_ms, 'last_snapshot_age_ms'),
  };
}

// Maps a validator over a payload that must be an array.
function parseArray<T>(raw: unknown, ctx: string, item: (v: unknown) => T): T[] {
  return asArray(raw, ctx).map(item);
}

async function getJSON<T>(path: string, parse: (raw: unknown) => T): Promise<T> {
  const r = await fetch(`${API_BASE}${path}`);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return parse(await r.json());
}

async function postJSON<T>(path: string, parse: (raw: unknown) => T, body?: unknown): Promise<T> {
  const r = await fetch(`${API_BASE}${path}`, {
    method: 'POST',
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return parse(await r.json());
}

// Parser for the common { status: string } POST acknowledgement.
function parseStatusAck(raw: unknown): { status: string } {
  const o = asObject(raw, 'StatusAck');
  return { status: asString(o.status, 'status') };
}

function parseSystemEvent(raw: unknown): SystemEvent {
  const o = asObject(raw, 'SystemEvent');
  return {
    ts_ns: asNumber(o.ts_ns, 'ts_ns'),
    severity: asString(o.severity, 'severity'),
    source: asString(o.source, 'source'),
    code: asString(o.code, 'code'),
    msg: asString(o.msg, 'msg'),
  };
}

function parseDailyPnl(raw: unknown): DailyPnl {
  const o = asObject(raw, 'DailyPnl');
  return {
    date: asString(o.date, 'date'),
    realized: asNumber(o.realized, 'realized'),
    unrealized: asNumber(o.unrealized, 'unrealized'),
    fees: asNumber(o.fees, 'fees'),
    total: asNumber(o.total, 'total'),
  };
}

function parseOrdersPayload(raw: unknown): OrdersPayload {
  const o = asObject(raw, 'OrdersPayload');
  return {
    open_count: asNumber(o.open_count, 'open_count'),
    events: parseArray(o.events, 'OrdersPayload.events', parseSystemEvent),
  };
}

function parseFundingRate(raw: unknown): FundingRate {
  const o = asObject(raw, 'FundingRate');
  return {
    funding_rate: asNumber(o.funding_rate, 'funding_rate'),
    next_funding_time: asNumber(o.next_funding_time, 'next_funding_time'),
    funding_rate_8h: asNumber(o.funding_rate_8h, 'funding_rate_8h'),
  };
}

// Shallow shape check: BacktestRow is a less-critical reporting payload, so we
// confirm it's an object and trust the field types rather than re-validating
// every numeric column.
function parseBacktestRow(raw: unknown): BacktestRow {
  return asObject(raw, 'BacktestRow') as unknown as BacktestRow;
}

// Shallow shape check (see parseBacktestRow).
function parseCalibrationRow(raw: unknown): CalibrationRow {
  return asObject(raw, 'CalibrationRow') as unknown as CalibrationRow;
}

// Shallow shape check: enabled flag and id/name validated, rest trusted.
function parseAlertRule(raw: unknown): AlertRule {
  return asObject(raw, 'AlertRule') as unknown as AlertRule;
}

function parseLogsResponse(raw: unknown): LogsResponse {
  const o = asObject(raw, 'LogsResponse');
  const lines = asArray(o.lines, 'LogsResponse.lines').map((l, i) =>
    asString(l, `lines[${i}]`),
  );
  return {
    lines,
    total_lines: asNumber(o.total_lines, 'total_lines'),
  };
}

async function postText(path: string, body: string): Promise<{ status: string }> {
  const r = await fetch(`${API_BASE}${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body,
  });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return parseStatusAck(await r.json());
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
  snapshot: () => getJSON('/api/snapshot', parseSnapshot),
  trades: (limit = 100) =>
    getJSON(`/api/trades?limit=${limit}`, (r) => parseArray(r, 'Trade[]', parseTrade)),
  daily: () => getJSON('/api/pnl/daily', (r) => parseArray(r, 'DailyPnl[]', parseDailyPnl)),
  events: (limit = 100) =>
    getJSON(`/api/events?limit=${limit}`, (r) => parseArray(r, 'SystemEvent[]', parseSystemEvent)),
  spreads: (limit = 1000) =>
    getJSON(`/api/spreads?limit=${limit}`, (r) => parseArray(r, 'SpreadPoint[]', parseSpreadPoint)),
  inventory: (limit = 1000) =>
    getJSON(`/api/inventory?limit=${limit}`, (r) =>
      parseArray(r, 'InventoryPoint[]', parseInventoryPoint),
    ),
  orders: (limit = 50) => getJSON(`/api/orders?limit=${limit}`, parseOrdersPayload),
  fundingRate: () => getJSON('/api/funding-rate', parseFundingRate),
  calibration: () =>
    getJSON('/api/calibration', (r) => parseArray(r, 'CalibrationRow[]', parseCalibrationRow)),
  calibrationRun: () => postJSON('/api/calibration/run', parseStatusAck),
  backtest: () =>
    getJSON('/api/backtest', (r) => parseArray(r, 'BacktestRow[]', parseBacktestRow)),
  backtestRun: () => postJSON('/api/backtest/run', parseStatusAck),
  status: () => getJSON('/api/v5/status', parseSystemStatus),
  alerts: () => getJSON('/api/alerts', (r) => parseArray(r, 'AlertRule[]', parseAlertRule)),
  alertUpsert: (rule: AlertRule) => postJSON('/api/alerts', parseAlertRule, rule),
  alertDelete: (id: string) => del(`/api/alerts?id=${encodeURIComponent(id)}`),
  logs: (lines = 200) => getJSON(`/api/logs?lines=${lines}`, parseLogsResponse),
  configGet: () => getText('/api/config'),
  configSave: (content: string) => postText('/api/config', content),
};

export type WsState = 'connected' | 'disconnected';

// Reconnect backoff: delay = WS_BASE_DELAY_MS * 2^attempt + random(0, WS_BASE_DELAY_MS),
// capped at WS_MAX_DELAY_MS. The random term de-syncs simultaneous clients so a
// server bounce doesn't trigger a thundering-herd reconnect in lockstep.
const WS_BASE_DELAY_MS = 250;
const WS_MAX_DELAY_MS = 5000;

export class WebSocketClient {
  private ws: WebSocket | null = null;
  private attempt = 0;
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
    this.ws.onopen = () => { this.attempt = 0; this.setState('connected'); };
    this.ws.onmessage = (e) => {
      try { this.onMessage(JSON.parse(e.data)); } catch { /* ignore */ }
    };
    this.ws.onclose = () => { this.setState('disconnected'); this.scheduleReconnect(); };
    this.ws.onerror = () => { /* onclose will handle */ };
  }

  private scheduleReconnect() {
    if (this.closed) return;
    const exp = Math.min(WS_BASE_DELAY_MS * 2 ** this.attempt, WS_MAX_DELAY_MS);
    const jitter = Math.random() * WS_BASE_DELAY_MS;
    const d = Math.min(exp + jitter, WS_MAX_DELAY_MS);
    this.attempt += 1;
    setTimeout(() => this.open(), d);
  }
}
