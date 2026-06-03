// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  api,
  WebSocketClient,
  type Snapshot,
  type Trade,
  type SystemEvent,
} from '../api';
import { useWsState } from '../WsStateContext';
import {
  StaleBanner,
  STALE_OPACITY,
  useFreshness,
} from '../components/freshness';
import { MetricCard } from '../components/MetricCard';
import { QuoteBar } from '../components/QuoteBar';
import { ProgressBar } from '../components/ProgressBar';
import { Pill } from '../components/Pill';
import { TradesTable } from '../components/TradesTable';
import { EventsFeed } from '../components/EventsFeed';

// Snapshot fields the backend may optionally expose. Using a loose extension type
// keeps strict TS happy while letting us read fields that aren't part of the
// formal Snapshot interface yet.
interface ExtSnapshot extends Snapshot {
  spread_bps?: number;
  max_inventory?: number;
  bid_price?: number;
  ask_price?: number;
  bid_qty?: number;
  ask_qty?: number;
  open_orders?: number;
  max_open_orders?: number;
  current_drawdown?: number;
  max_drawdown_pct?: number;
  circuit_breaker?: string;
  fill_count_10s?: number;
  fill_count_60s?: number;
  maker_ratio?: number;
  avg_spread_bps?: number;
  fills_per_10s?: number;
  volatility?: number;
  gamma?: number;
  k?: number;
  t?: number;
  lat_p50_us?: number;
  lat_p95_us?: number;
  lat_p99_us?: number;
}

const priceFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});
const moneyFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});
const btcFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 4,
  maximumFractionDigits: 4,
  signDisplay: 'exceptZero',
});
const btcUnsignedFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 4,
  maximumFractionDigits: 4,
});
const pctFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 1,
  maximumFractionDigits: 1,
});
const bpsFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 3,
});

function formatPrice(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return priceFmt.format(n);
}

function signTone(n: number): 'green' | 'red' | 'muted' {
  if (!Number.isFinite(n) || n === 0) return 'muted';
  return n > 0 ? 'green' : 'red';
}

function signedMoney(n: number): string {
  if (!Number.isFinite(n)) return '—';
  const sign = n > 0 ? '+' : n < 0 ? '−' : '';
  return `${sign}$${moneyFmt.format(Math.abs(n))}`;
}

function plainMoney(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return `$${moneyFmt.format(n)}`;
}

function fmtLatUs(us: number | undefined): string {
  if (us === undefined || !Number.isFinite(us) || us <= 0) return '—';
  if (us < 1000) return `${us.toFixed(0)} µs`;
  return `${(us / 1000).toFixed(2)} ms`;
}

export default function Dashboard() {
  const { wsState, setWsState } = useWsState();
  const [snap, setSnap] = useState<ExtSnapshot | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [events, setEvents] = useState<SystemEvent[]>([]);
  const { stale, markSuccess, markError } = useFreshness();

  useEffect(() => {
    const ws = new WebSocketClient(
      (s) => {
        setSnap(s as ExtSnapshot);
        markSuccess();
      },
      (state) => setWsState(state),
    );
    ws.connect();
    api
      .snapshot()
      .then((s) => {
        setSnap(s as ExtSnapshot);
        markSuccess();
      })
      .catch(() => markError());
    const poll = () => {
      api
        .trades(20)
        .then((t) => setTrades(t.slice(0, 20)))
        .catch(() => markError());
      api
        .events(50)
        .then((e) => setEvents(e.slice(0, 50)))
        .catch(() => markError());
    };
    poll();
    const id = setInterval(poll, 5000);
    return () => {
      clearInterval(id);
      ws.close();
    };
  }, [setWsState, markSuccess, markError]);

  const s = snap;

  const bid = useMemo(() => {
    if (!s) return null;
    if (typeof s.bid_price === 'number') {
      return { price: s.bid_price, qty: typeof s.bid_qty === 'number' ? s.bid_qty : 0 };
    }
    return null;
  }, [s]);

  const ask = useMemo(() => {
    if (!s) return null;
    if (typeof s.ask_price === 'number') {
      return { price: s.ask_price, qty: typeof s.ask_qty === 'number' ? s.ask_qty : 0 };
    }
    return null;
  }, [s]);

  const inventory = s?.inventory ?? 0;
  const maxInventory = s?.max_inventory && s.max_inventory > 0 ? s.max_inventory : 1;
  const realized = s?.realized_pnl ?? 0;
  const unrealized = s?.unrealized_pnl ?? 0;
  const fees = s?.total_fees ?? 0;
  const cumTotal = s?.cum_total ?? 0;
  const mid = s?.mid_price ?? NaN;
  const spreadBps = s?.spread_bps;
  const avgEntry = s?.avg_entry ?? 0;
  const cb = (s?.circuit_breaker ?? 'ok').toLowerCase();
  const cbHalted = cb === 'halt' || cb === 'halted' || cb === 'tripped';
  const drawdown = s?.current_drawdown ?? 0;
  const maxDrawdown = s?.max_drawdown_pct && s.max_drawdown_pct > 0 ? s.max_drawdown_pct : 5;
  const openOrders = s?.open_orders ?? 0;
  const maxOpenOrders =
    s?.max_open_orders && s.max_open_orders > 0 ? s.max_open_orders : 10;
  const fillCount = s?.fill_count_60s ?? 0;
  const makerRatio = s?.maker_ratio;
  const avgSpreadBps = s?.avg_spread_bps;
  const fillsPer10s = s?.fills_per_10s;
  const volatility = s?.volatility;
  const gamma = s?.gamma;
  const kParam = s?.k;
  const tHorizon = s?.t;
  const latP50 = s?.lat_p50_us;
  const latP95 = s?.lat_p95_us;
  const latP99 = s?.lat_p99_us;

  const invTone = signTone(inventory);

  // The dashboard consumes the live WS feed. A dropped WS connection means the
  // displayed values are no longer live, so treat it the same as stale data:
  // dim the panels and surface an explicit overlay.
  const disconnected = wsState !== 'connected';
  const dimmed = stale || disconnected;
  const dimStyle = { opacity: dimmed ? STALE_OPACITY : 1 };

  return (
    <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
      {disconnected && (
        <div className="disconnected-overlay">
          <span>WS DISCONNECTED — RECONNECTING…</span>
        </div>
      )}

      <StaleBanner show={stale && !disconnected} />

      <div className="section" style={dimStyle}>
        <div className="row-4">
          <MetricCard label="Mid">
            <div className="metric-value">{Number.isFinite(mid) ? formatPrice(mid) : '—'}</div>
            <div className="metric-sub">
              SPREAD <span className="mono">{spreadBps !== undefined ? bpsFmt.format(spreadBps) : '—'}</span> bps
            </div>
          </MetricCard>

          <MetricCard label="Inventory">
            <div className={`metric-value ${invTone}`}>
              {Number.isFinite(inventory) ? btcFmt.format(inventory) : '—'}
            </div>
            <ProgressBar
              value={inventory}
              max={maxInventory}
              tone={inventory > 0 ? 'green' : inventory < 0 ? 'red' : 'neutral'}
              ratioText={`${btcUnsignedFmt.format(Math.abs(inventory))} / ${btcUnsignedFmt.format(maxInventory)}`}
            />
          </MetricCard>

          <MetricCard label="Realized P&L">
            <div className={`metric-value ${signTone(realized)}`}>{signedMoney(realized)}</div>
            <div className="metric-sub">
              FEES <span className="mono">{plainMoney(fees)}</span>
            </div>
          </MetricCard>

          <MetricCard label="Circuit Breaker">
            <div>
              <Pill tone={cbHalted ? 'red' : 'green'}>{cbHalted ? 'HALT' : 'OK'}</Pill>
            </div>
            <div className="metric-sub-rows">
              <ProgressBar
                label="DD"
                value={drawdown}
                max={maxDrawdown}
                tone="amber"
                ratioText={`${pctFmt.format(drawdown)}% / ${pctFmt.format(maxDrawdown)}%`}
              />
              <ProgressBar
                label="OOO"
                value={openOrders}
                max={maxOpenOrders}
                tone="neutral"
                ratioText={`${openOrders} / ${maxOpenOrders}`}
              />
            </div>
          </MetricCard>
        </div>
      </div>

      <div className="section" style={dimStyle}>
        <QuoteBar
          bid={bid}
          ask={ask}
          formatPrice={formatPrice}
          formatQty={(n) => btcUnsignedFmt.format(n)}
        />
      </div>

      <div className="section" style={dimStyle}>
        <div className="row-4">
          <MetricCard label="Unrealized P&L">
            <div className={`metric-value ${signTone(unrealized)}`}>{signedMoney(unrealized)}</div>
            <div className="metric-sub-rows">
              <div className="sub-row">
                <span>Avg Entry</span>
                <span className="val">{avgEntry > 0 ? formatPrice(avgEntry) : '—'}</span>
              </div>
              <div className="sub-row">
                <span>Mark</span>
                <span className="val">{Number.isFinite(mid) ? formatPrice(mid) : '—'}</span>
              </div>
              <div className="sub-row">
                <span>Cum Total</span>
                <span className="val">{signedMoney(cumTotal)}</span>
              </div>
            </div>
          </MetricCard>

          <MetricCard label="Fills (60s)">
            <div className="metric-value">{fillCount}</div>
            <div className="metric-sub-rows">
              <div className="sub-row">
                <span>Maker Ratio</span>
                <span className="val">
                  {makerRatio !== undefined ? `${pctFmt.format(makerRatio * 100)}%` : '—'}
                </span>
              </div>
              <div className="sub-row">
                <span>Avg Spread</span>
                <span className="val">
                  {avgSpreadBps !== undefined ? `${bpsFmt.format(avgSpreadBps)} bps` : '—'}
                </span>
              </div>
              <div className="sub-row">
                <span>Fills / 10s</span>
                <span className="val">{fillsPer10s !== undefined ? fillsPer10s : '—'}</span>
              </div>
            </div>
          </MetricCard>

          <MetricCard label="Latency">
            <div className={`metric-value ${latP95 === undefined ? 'muted' : ''}`}>
              {fmtLatUs(latP95)}
            </div>
            <div className="metric-sub-rows">
              <div className="sub-row">
                <span>P50</span>
                <span className={`val ${latP50 === undefined ? 'dim' : ''}`}>{fmtLatUs(latP50)}</span>
              </div>
              <div className="sub-row">
                <span>P95</span>
                <span className={`val ${latP95 === undefined ? 'dim' : ''}`}>{fmtLatUs(latP95)}</span>
              </div>
              <div className="sub-row">
                <span>P99</span>
                <span className={`val ${latP99 === undefined ? 'dim' : ''}`}>{fmtLatUs(latP99)}</span>
              </div>
              <div className="sub-row">
                <span>WS Lag</span>
                <span className="val dim">—</span>
              </div>
            </div>
          </MetricCard>

          <MetricCard label="A-S Parameters">
            <div className={`metric-value ${gamma === undefined ? 'muted' : ''}`}>
              γ {gamma !== undefined ? gamma.toFixed(3) : '—'}
            </div>
            <div className="metric-sub-rows">
              <div className="sub-row">
                <span>k</span>
                <span className={`val ${kParam === undefined ? 'dim' : ''}`}>
                  {kParam !== undefined ? kParam.toFixed(3) : '—'}
                </span>
              </div>
              <div className="sub-row">
                <span>T</span>
                <span className={`val ${tHorizon === undefined ? 'dim' : ''}`}>
                  {tHorizon !== undefined ? tHorizon.toFixed(2) : '—'}
                </span>
              </div>
              <div className="sub-row">
                <span>Realized Vol</span>
                <span className="val">
                  {volatility !== undefined ? pctFmt.format(volatility) : '—'}
                </span>
              </div>
            </div>
          </MetricCard>
        </div>
      </div>

      <div className="section" style={{ flex: 1, ...dimStyle }}>
        <div className="row-2">
          <TradesTable trades={trades} formatPrice={formatPrice} />
          <EventsFeed events={events} />
        </div>
      </div>

      <footer className="footer">
        <span>SPREADARA v0.11.9 · DSLabs</span>
        <span className={`right ${wsState}`}>
          {wsState === 'connected' ? 'CONNECTED' : 'DISCONNECTED'}
        </span>
      </footer>
    </div>
  );
}
