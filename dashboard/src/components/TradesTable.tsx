// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import React from 'react';
import type { Trade } from '../api';
import { Pill } from './Pill';

interface Props {
  trades: Trade[];
  formatPrice: (n: number) => string;
}

function parseTsNs(ts: number | undefined): Date | null {
  if (typeof ts !== 'number' || !Number.isFinite(ts) || ts <= 0) return null;
  const ms = ts > 1e14 ? ts / 1e6 : ts;
  const d = new Date(ms);
  return isNaN(d.getTime()) ? null : d;
}

function fmtTime(ts: number | undefined): string {
  const d = parseTsNs(ts);
  if (!d) return '—';
  const h = String(d.getUTCHours()).padStart(2, '0');
  const m = String(d.getUTCMinutes()).padStart(2, '0');
  const s = String(d.getUTCSeconds()).padStart(2, '0');
  const ms = String(d.getUTCMilliseconds()).padStart(3, '0');
  return `${h}:${m}:${s}.${ms}`;
}

const qtyFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 4,
  maximumFractionDigits: 4,
});

export function TradesTable({ trades, formatPrice }: Props) {
  return (
    <div className="panel">
      <div className="panel-header">
        <span>Last Trades</span>
        <span className="count">{trades.length}</span>
      </div>
      <div className="panel-body">
        <table className="table">
          <thead>
            <tr>
              <th className="col-time">Time</th>
              <th className="col-side">Side</th>
              <th className="col-price">Price</th>
              <th className="col-qty">Qty</th>
            </tr>
          </thead>
          <tbody>
            {trades.length === 0 ? (
              <tr className="empty-row">
                <td colSpan={4}>—</td>
              </tr>
            ) : (
              trades.map((t, i) => {
                const isBuy = t.side > 0;
                return (
                  <tr key={`${t.order_id}-${i}`}>
                    <td className="col-time">{fmtTime(t.ts_ns)}</td>
                    <td className="col-side">
                      <Pill tone={isBuy ? 'green' : 'red'}>{isBuy ? 'BUY' : 'SELL'}</Pill>
                    </td>
                    <td className="col-price">{formatPrice(t.price)}</td>
                    <td className="col-qty">{qtyFmt.format(t.qty)}</td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
