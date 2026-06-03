// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  Bar,
  BarChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { api, type DailyPnl, type Trade } from '../api';
import { COLORS, plainMoney, utcDayKey } from './fmt';

export default function FeeAnalysis() {
  const [trades, setTrades] = useState<Trade[]>([]);
  const [daily, setDaily] = useState<DailyPnl[]>([]);

  useEffect(() => {
    api.trades(500).then(setTrades).catch(() => {});
    api.daily().then(setDaily).catch(() => {});
  }, []);

  const stats = useMemo(() => {
    let total = 0;
    let maker = 0;
    let taker = 0;
    for (const t of trades) {
      total += t.fee;
      if (t.is_maker) maker += t.fee;
      else taker += t.fee;
    }
    const grossPnl = daily.reduce((s, d) => s + d.realized, 0);
    const pctOfGross = grossPnl !== 0 ? (total / Math.abs(grossPnl)) * 100 : 0;
    return { total, maker, taker, pctOfGross };
  }, [trades, daily]);

  const byDay = useMemo(() => {
    const m = new Map<string, number>();
    for (const t of trades) {
      const key = utcDayKey(t.ts_ns);
      m.set(key, (m.get(key) ?? 0) + t.fee);
    }
    return Array.from(m.entries())
      .map(([date, fees]) => ({ date, fees }))
      .sort((a, b) => a.date.localeCompare(b.date));
  }, [trades]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Fee Analysis</h1>
      </div>

      <div className="section">
        <div className="row-4">
          <Card label="Total Fees" value={plainMoney(stats.total)} />
          <Card label="Maker Fees" value={plainMoney(stats.maker)} sub={pct(stats.maker, stats.total)} />
          <Card label="Taker Fees" value={plainMoney(stats.taker)} sub={pct(stats.taker, stats.total)} />
          <Card
            label="Fees % of Gross"
            value={Number.isFinite(stats.pctOfGross) ? `${stats.pctOfGross.toFixed(2)}%` : '—'}
          />
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header">
            <span>FEES BY DAY</span>
            <span className="count">{byDay.length}</span>
          </div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={280}>
              <BarChart data={byDay} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="date" stroke={COLORS.muted} fontSize={10} />
                <YAxis stroke={COLORS.muted} fontSize={10} tickFormatter={(v) => `$${v}`} />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v) => plainMoney(Number(v))}
                />
                <Bar dataKey="fees" fill={COLORS.amber} radius={[2, 2, 0, 0]} />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}

function pct(part: number, total: number): string {
  if (total <= 0) return '—';
  return `${((part / total) * 100).toFixed(1)}%`;
}

function Card({ label, value, sub }: { label: string; value: string; sub?: string }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className="metric-value">{value}</div>
      {sub && <div className="metric-sub">{sub}</div>}
    </div>
  );
}
