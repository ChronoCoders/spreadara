// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  Bar,
  BarChart,
  CartesianGrid,
  Cell,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { api, type DailyPnl } from '../api';
import { COLORS, plainMoney, signedMoney } from './fmt';

export default function PnL() {
  const [daily, setDaily] = useState<DailyPnl[]>([]);
  const [tradesCount, setTradesCount] = useState<number>(0);

  useEffect(() => {
    api.daily().then(setDaily).catch(() => {});
    api.trades(500).then((t) => setTradesCount(t.length)).catch(() => {});
  }, []);

  const asc = useMemo(
    () => [...daily].sort((a, b) => a.date.localeCompare(b.date)),
    [daily],
  );

  const cumulative = useMemo(() => {
    let c = 0;
    return asc.map((d) => {
      c += d.total;
      return { date: d.date, cum: c };
    });
  }, [asc]);

  const totals = useMemo(() => {
    const realized = daily.reduce((s, d) => s + d.realized, 0);
    const fees = daily.reduce((s, d) => s + d.fees, 0);
    let best: DailyPnl | null = null;
    let worst: DailyPnl | null = null;
    for (const d of daily) {
      if (!best || d.total > best.total) best = d;
      if (!worst || d.total < worst.total) worst = d;
    }
    return { realized, fees, best, worst };
  }, [daily]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">P&amp;L</h1>
      </div>

      <div className="section">
        <div className="row-4">
          <SummaryCard label="Total Realized" value={signedMoney(totals.realized)} tone={tone(totals.realized)} />
          <SummaryCard label="Total Fees" value={plainMoney(totals.fees)} />
          <SummaryCard label="Trade Count" value={String(tradesCount)} />
          <SummaryCard
            label="Best Day"
            value={totals.best ? signedMoney(totals.best.total) : '—'}
            sub={totals.best?.date}
            tone="green"
          />
        </div>
      </div>

      <div className="section">
        <div className="row-2">
          <SummaryCard
            label="Worst Day"
            value={totals.worst ? signedMoney(totals.worst.total) : '—'}
            sub={totals.worst?.date}
            tone="red"
          />
          <SummaryCard label="Days Tracked" value={String(daily.length)} />
        </div>
      </div>

      <div className="section">
        <div className="panel">
          <div className="panel-header">
            <span>DAILY P&amp;L</span>
            <span className="count">{daily.length}</span>
          </div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={260}>
              <BarChart data={asc} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="date" stroke={COLORS.muted} fontSize={10} />
                <YAxis stroke={COLORS.muted} fontSize={10} tickFormatter={(v) => `$${v}`} />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v) => signedMoney(Number(v))}
                />
                <Bar dataKey="total" radius={[2, 2, 0, 0]}>
                  {asc.map((d, i) => (
                    <Cell key={i} fill={d.total >= 0 ? COLORS.green : COLORS.red} />
                  ))}
                </Bar>
              </BarChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header">
            <span>CUMULATIVE P&amp;L</span>
          </div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={260}>
              <LineChart data={cumulative} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="date" stroke={COLORS.muted} fontSize={10} />
                <YAxis stroke={COLORS.muted} fontSize={10} tickFormatter={(v) => `$${v}`} />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v) => signedMoney(Number(v))}
                />
                <Line type="monotone" dataKey="cum" stroke={COLORS.amber} strokeWidth={2} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}

function tone(n: number): 'green' | 'red' | undefined {
  if (!Number.isFinite(n) || n === 0) return undefined;
  return n > 0 ? 'green' : 'red';
}

function SummaryCard({
  label,
  value,
  sub,
  tone,
}: {
  label: string;
  value: string;
  sub?: string;
  tone?: 'green' | 'red';
}) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className={`metric-value ${tone ?? ''}`}>{value}</div>
      {sub && <div className="metric-sub">{sub}</div>}
    </div>
  );
}
