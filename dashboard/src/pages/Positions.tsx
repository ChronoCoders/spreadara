// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import { api, type InventoryPoint, type Snapshot } from '../api';
import { fmtTimeNs, plainMoney, priceFmt, qtyFmt, signedMoney } from './fmt';

export default function Positions() {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [history, setHistory] = useState<InventoryPoint[]>([]);

  useEffect(() => {
    const fetch = () => {
      api.snapshot().then(setSnap).catch(() => {});
      api.inventory(100).then(setHistory).catch(() => {});
    };
    fetch();
    const id = setInterval(fetch, 2000);
    return () => clearInterval(id);
  }, []);

  // Show only entries where inventory changed vs the previous sample.
  const changes = useMemo(() => {
    const asc = [...history].sort((a, b) => a.ts_ns - b.ts_ns);
    const out: { ts_ns: number; delta: number; inventory: number; mid: number }[] = [];
    let prev: number | null = null;
    for (const p of asc) {
      if (prev !== null && Math.abs(p.inventory - prev) > 1e-9) {
        out.push({ ts_ns: p.ts_ns, delta: p.inventory - prev, inventory: p.inventory, mid: p.mid_price });
      }
      prev = p.inventory;
    }
    return out.reverse();
  }, [history]);

  const inv = snap?.inventory ?? 0;
  const avgEntry = snap?.avg_entry ?? 0;
  const mid = snap?.mid_price ?? 0;
  const unrealized = snap?.unrealized_pnl ?? 0;

  const direction = inv > 0 ? 'LONG' : inv < 0 ? 'SHORT' : 'FLAT';
  const dirColor = inv > 0 ? 'var(--green)' : inv < 0 ? 'var(--red)' : undefined;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Positions</h1>
      </div>

      <div className="section">
        <div className="row-4">
          <div className="metric">
            <div className="metric-label">Direction</div>
            <div className="metric-value" style={{ color: dirColor }}>{direction}</div>
            <div className="metric-sub">size {qtyFmt.format(Math.abs(inv))} BTC</div>
          </div>
          <div className="metric">
            <div className="metric-label">Avg Entry</div>
            <div className="metric-value">{avgEntry > 0 ? priceFmt.format(avgEntry) : '—'}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Mark</div>
            <div className="metric-value">{mid > 0 ? priceFmt.format(mid) : '—'}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Unrealized P&amp;L</div>
            <div
              className="metric-value"
              style={{ color: unrealized > 0 ? 'var(--green)' : unrealized < 0 ? 'var(--red)' : undefined }}
            >
              {signedMoney(unrealized)}
            </div>
            <div className="metric-sub">{plainMoney(Math.abs(unrealized))} absolute</div>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header">
            <span>RECENT POSITION CHANGES</span>
            <span className="count">{changes.length}</span>
          </div>
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th className="col-time">Time</th>
                  <th className="col-qty">Delta</th>
                  <th className="col-qty">New Inv.</th>
                  <th className="col-qty">Mid</th>
                </tr>
              </thead>
              <tbody>
                {changes.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={4}>no recent changes</td>
                  </tr>
                ) : (
                  changes.map((c) => (
                    <tr key={c.ts_ns}>
                      <td className="col-time">{fmtTimeNs(c.ts_ns)}</td>
                      <td
                        className="col-qty"
                        style={{ color: c.delta > 0 ? 'var(--green)' : 'var(--red)' }}
                      >
                        {c.delta > 0 ? '+' : ''}
                        {qtyFmt.format(c.delta)}
                      </td>
                      <td className="col-qty">{qtyFmt.format(c.inventory)}</td>
                      <td className="col-qty">{c.mid > 0 ? priceFmt.format(c.mid) : '—'}</td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}
