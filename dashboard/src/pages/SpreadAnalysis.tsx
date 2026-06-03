// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  Bar,
  BarChart,
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { api, type SpreadPoint } from '../api';
import { bpsFmt, COLORS, fmtTimeNs, quantile } from './fmt';

export default function SpreadAnalysis() {
  const [points, setPoints] = useState<SpreadPoint[]>([]);

  useEffect(() => {
    api.spreads(1000).then(setPoints).catch(() => {});
  }, []);

  const asc = useMemo(
    () => [...points].sort((a, b) => a.ts_ns - b.ts_ns),
    [points],
  );

  const series = useMemo(
    () => asc.map((p) => ({ t: fmtTimeNs(p.ts_ns), spread: p.spread_bps })),
    [asc],
  );

  const stats = useMemo(() => {
    if (asc.length === 0) {
      return { min: NaN, max: NaN, avg: NaN, p50: NaN, p95: NaN };
    }
    const vals = asc.map((p) => p.spread_bps);
    const sorted = [...vals].sort((a, b) => a - b);
    const avg = vals.reduce((s, v) => s + v, 0) / vals.length;
    return {
      min: sorted[0],
      max: sorted[sorted.length - 1],
      avg,
      p50: quantile(sorted, 0.5),
      p95: quantile(sorted, 0.95),
    };
  }, [asc]);

  const histogram = useMemo(() => {
    if (asc.length === 0) return [];
    const vals = asc.map((p) => p.spread_bps);
    const min = Math.min(...vals);
    const max = Math.max(...vals);
    const buckets = 20;
    if (max <= min) return [{ bin: min.toFixed(3), count: vals.length }];
    const width = (max - min) / buckets;
    const counts = new Array(buckets).fill(0);
    for (const v of vals) {
      const idx = Math.min(buckets - 1, Math.floor((v - min) / width));
      counts[idx]++;
    }
    return counts.map((c, i) => ({
      bin: (min + i * width).toFixed(3),
      count: c,
    }));
  }, [asc]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Spread Analysis</h1>
        <span className="page-count">{points.length} samples</span>
      </div>

      <div className="section">
        <div className="row-4">
          <Card label="Min" value={fmtBps(stats.min)} />
          <Card label="Max" value={fmtBps(stats.max)} />
          <Card label="Avg" value={fmtBps(stats.avg)} />
          <Card label="P50 / P95" value={`${fmtBps(stats.p50)} / ${fmtBps(stats.p95)}`} />
        </div>
      </div>

      <div className="section">
        <div className="panel">
          <div className="panel-header"><span>SPREAD OVER TIME (bps)</span></div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={240}>
              <LineChart data={series} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="t" stroke={COLORS.muted} fontSize={10} minTickGap={40} />
                <YAxis stroke={COLORS.muted} fontSize={10} tickFormatter={(v) => bpsFmt.format(v)} />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v) => `${bpsFmt.format(Number(v))} bps`}
                />
                <Line type="monotone" dataKey="spread" stroke={COLORS.amber} strokeWidth={1.5} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>DISTRIBUTION</span></div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={240}>
              <BarChart data={histogram} margin={{ top: 12, right: 16, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="bin" stroke={COLORS.muted} fontSize={10} minTickGap={20} />
                <YAxis stroke={COLORS.muted} fontSize={10} />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                />
                <Bar dataKey="count" fill={COLORS.amber} />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}

function fmtBps(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return `${bpsFmt.format(n)} bps`;
}

function Card({ label, value }: { label: string; value: string }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className="metric-value">{value}</div>
    </div>
  );
}
