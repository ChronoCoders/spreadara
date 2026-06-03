// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import { api, type Trade } from '../api';
import { Pill } from '../components/Pill';
import { fmtTimeNs, plainMoney, priceFmt, qtyFmt } from './fmt';

type SideFilter = 'all' | 'buy' | 'sell';
type SortDir = 'asc' | 'desc';

export default function Trades() {
  const [trades, setTrades] = useState<Trade[]>([]);
  const [side, setSide] = useState<SideFilter>('all');
  const [dir, setDir] = useState<SortDir>('desc');

  useEffect(() => {
    api.trades(500).then(setTrades).catch(() => {});
  }, []);

  const filtered = useMemo(() => {
    let list = trades;
    if (side !== 'all') {
      const want = side === 'buy' ? 1 : -1;
      list = list.filter((t) => t.side === want);
    }
    return [...list].sort((a, b) => (dir === 'desc' ? b.ts_ns - a.ts_ns : a.ts_ns - b.ts_ns));
  }, [trades, side, dir]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Trades</h1>
        <div className="page-toolbar">
          <FilterButton label="All" active={side === 'all'} onClick={() => setSide('all')} />
          <FilterButton label="Buy" active={side === 'buy'} onClick={() => setSide('buy')} />
          <FilterButton label="Sell" active={side === 'sell'} onClick={() => setSide('sell')} />
          <span className="page-count">{filtered.length} of {trades.length}</span>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th
                    className="col-time"
                    style={{ cursor: 'pointer' }}
                    onClick={() => setDir(dir === 'desc' ? 'asc' : 'desc')}
                  >
                    Time {dir === 'desc' ? '↓' : '↑'}
                  </th>
                  <th className="col-side">Side</th>
                  <th className="col-price">Price</th>
                  <th className="col-qty">Qty</th>
                  <th className="col-qty">Fee</th>
                  <th className="col-side">Maker</th>
                </tr>
              </thead>
              <tbody>
                {filtered.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={6}>no trades</td>
                  </tr>
                ) : (
                  filtered.map((t, i) => (
                    <tr key={`${t.ts_ns}-${t.order_id}-${i}`}>
                      <td className="col-time">{fmtTimeNs(t.ts_ns)}</td>
                      <td className="col-side">
                        <Pill tone={t.side > 0 ? 'green' : 'red'}>{t.side > 0 ? 'BUY' : 'SELL'}</Pill>
                      </td>
                      <td className="col-price">{priceFmt.format(t.price)}</td>
                      <td className="col-qty">{qtyFmt.format(t.qty)}</td>
                      <td className="col-qty">{plainMoney(t.fee)}</td>
                      <td className="col-side">{t.is_maker ? '✓' : '✗'}</td>
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

function FilterButton({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  return (
    <button className={`filter-btn ${active ? 'filter-btn-active' : ''}`} onClick={onClick}>
      {label}
    </button>
  );
}
