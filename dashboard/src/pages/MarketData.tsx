// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { api, type InventoryPoint, type Snapshot } from '../api';
import { bpsFmt, COLORS, fmtTimeNs, priceFmt, qtyFmt } from './fmt';
import {
  StaleBanner,
  STALE_OPACITY,
  useFreshness,
} from '../components/freshness';

interface ExtSnap extends Snapshot {
  bid_price?: number;
  ask_price?: number;
  bid_qty?: number;
  ask_qty?: number;
  spread_bps?: number;
}

export default function MarketData() {
  const [snap, setSnap] = useState<ExtSnap | null>(null);
  const [points, setPoints] = useState<InventoryPoint[]>([]);
  const { stale, markSuccess, markError } = useFreshness();

  useEffect(() => {
    const fetch = () => {
      api
        .snapshot()
        .then((s) => {
          setSnap(s as ExtSnap);
          markSuccess();
        })
        .catch(() => markError());
      api.inventory(200).then(setPoints).catch(() => markError());
    };
    fetch();
    const id = setInterval(fetch, 2000);
    return () => clearInterval(id);
  }, [markSuccess, markError]);

  const series = useMemo(
    () =>
      [...points]
        .sort((a, b) => a.ts_ns - b.ts_ns)
        .map((p) => ({ t: fmtTimeNs(p.ts_ns), mid: p.mid_price })),
    [points],
  );

  const bid = snap?.bid_price;
  const ask = snap?.ask_price;
  const mid = snap?.mid_price;
  const spread = snap?.spread_bps;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Market Data</h1>
      </div>

      <StaleBanner show={stale} />

      <div className="section" style={{ opacity: stale ? STALE_OPACITY : 1 }}>
        <div className="row-4">
          <Card label="Bid" value={fmtPrice(bid)} sub={`${fmtQty(snap?.bid_qty)} BTC`} tone="green" />
          <Card label="Ask" value={fmtPrice(ask)} sub={`${fmtQty(snap?.ask_qty)} BTC`} tone="red" />
          <Card label="Mid" value={fmtPrice(mid)} />
          <Card label="Spread" value={spread !== undefined ? `${bpsFmt.format(spread)} bps` : '—'} />
        </div>
      </div>

      <div className="section" style={{ opacity: stale ? STALE_OPACITY : 1 }}>
        <div className="panel">
          <div className="panel-header"><span>BID / ASK GAP</span></div>
          <div style={{ padding: 24, background: 'var(--bg-surface)' }}>
            <SpreadBar bid={bid} ask={ask} />
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1, opacity: stale ? STALE_OPACITY : 1 }}>
        <div className="panel">
          <div className="panel-header"><span>MID TREND</span></div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={300}>
              <LineChart data={series} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="t" stroke={COLORS.muted} fontSize={10} minTickGap={40} />
                <YAxis
                  stroke={COLORS.muted}
                  fontSize={10}
                  domain={['auto', 'auto']}
                  tickFormatter={(v) => priceFmt.format(v)}
                />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v) => priceFmt.format(Number(v))}
                />
                <Line type="monotone" dataKey="mid" stroke={COLORS.amber} strokeWidth={1.5} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}

function SpreadBar({ bid, ask }: { bid?: number; ask?: number }) {
  if (bid === undefined || ask === undefined || bid <= 0 || ask <= 0) {
    return <div style={{ color: 'var(--text-muted)', fontFamily: 'var(--font-mono)' }}>no quote</div>;
  }
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 16, fontFamily: 'var(--font-mono)' }}>
      <span style={{ color: 'var(--green)', fontSize: 16 }}>{priceFmt.format(bid)}</span>
      <div
        style={{
          flex: 1,
          height: 10,
          borderRadius: 2,
          background:
            `linear-gradient(to right, ${COLORS.green}, ${COLORS.green}33 30%, ${COLORS.red}33 70%, ${COLORS.red})`,
        }}
      />
      <span style={{ color: 'var(--red)', fontSize: 16 }}>{priceFmt.format(ask)}</span>
    </div>
  );
}

function Card({ label, value, sub, tone }: { label: string; value: string; sub?: string; tone?: 'green' | 'red' }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className={`metric-value ${tone ?? ''}`}>{value}</div>
      {sub && <div className="metric-sub">{sub}</div>}
    </div>
  );
}

function fmtPrice(n?: number): string {
  return n !== undefined && Number.isFinite(n) ? priceFmt.format(n) : '—';
}
function fmtQty(n?: number): string {
  return n !== undefined && Number.isFinite(n) ? qtyFmt.format(n) : '—';
}
