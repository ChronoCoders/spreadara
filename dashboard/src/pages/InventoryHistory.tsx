// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { api, type InventoryPoint } from '../api';
import { COLORS, fmtTimeNs, priceFmt, qtyFmt } from './fmt';

export default function InventoryHistory() {
  const [points, setPoints] = useState<InventoryPoint[]>([]);

  useEffect(() => {
    api.inventory(1000).then(setPoints).catch(() => {});
  }, []);

  const asc = useMemo(
    () => [...points].sort((a, b) => a.ts_ns - b.ts_ns),
    [points],
  );

  const series = useMemo(
    () =>
      asc.map((p) => ({
        t: fmtTimeNs(p.ts_ns),
        inventory: p.inventory,
        mid: p.mid_price,
      })),
    [asc],
  );

  const summary = useMemo(() => {
    let long = 0;
    let short = 0;
    let flat = 0;
    for (const p of asc) {
      if (p.inventory > 0) long++;
      else if (p.inventory < 0) short++;
      else flat++;
    }
    return { long, short, flat };
  }, [asc]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Inventory History</h1>
        <span className="page-count">{points.length} samples</span>
      </div>

      <div className="section">
        <div className="row-4">
          <Card label="Long Samples" value={String(summary.long)} tone="green" />
          <Card label="Short Samples" value={String(summary.short)} tone="red" />
          <Card label="Flat Samples" value={String(summary.flat)} />
          <Card
            label="Long Ratio"
            value={asc.length > 0 ? `${((summary.long / asc.length) * 100).toFixed(1)}%` : '—'}
          />
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>INVENTORY vs MID</span></div>
          <div className="chart-body">
            <ResponsiveContainer width="100%" height={360}>
              <LineChart data={series} margin={{ top: 12, right: 32, bottom: 8, left: 8 }}>
                <CartesianGrid stroke={COLORS.border} vertical={false} />
                <XAxis dataKey="t" stroke={COLORS.muted} fontSize={10} minTickGap={40} />
                <YAxis
                  yAxisId="inv"
                  stroke={COLORS.green}
                  fontSize={10}
                  tickFormatter={(v) => qtyFmt.format(v)}
                />
                <YAxis
                  yAxisId="mid"
                  orientation="right"
                  stroke={COLORS.muted}
                  fontSize={10}
                  tickFormatter={(v) => priceFmt.format(v)}
                  domain={['auto', 'auto']}
                />
                <Tooltip
                  contentStyle={{ background: COLORS.surface, border: `1px solid ${COLORS.border}`, fontSize: 11 }}
                  formatter={(v, name) => (name === 'mid' ? priceFmt.format(Number(v)) : qtyFmt.format(Number(v)))}
                />
                <ReferenceLine yAxisId="inv" y={0} stroke={COLORS.muted} strokeDasharray="2 4" />
                <Line
                  yAxisId="inv"
                  type="monotone"
                  dataKey="inventory"
                  stroke={COLORS.green}
                  strokeWidth={1.5}
                  dot={false}
                />
                <Line
                  yAxisId="mid"
                  type="monotone"
                  dataKey="mid"
                  stroke={COLORS.amber}
                  strokeWidth={1}
                  dot={false}
                  opacity={0.8}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}

function Card({ label, value, tone }: { label: string; value: string; tone?: 'green' | 'red' }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className={`metric-value ${tone ?? ''}`}>{value}</div>
    </div>
  );
}
