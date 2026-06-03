// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import { api, type DailyPnl, type Trade } from '../api';
import { fmtTimeNs, plainMoney, signedMoney, utcDayKey } from './fmt';

interface TradePnl {
  trade: Trade;
  realized: number;
}

interface DaySummary {
  date: string;
  count: number;
  netPnl: number;
  fees: number;
  best?: TradePnl;
  worst?: TradePnl;
}

// Walk trades oldest-to-newest with an avg-cost basis to derive per-trade
// realized P&L. WHY: best/worst trade per day is not in the API; computing
// it client-side matches what the C++ PositionTracker does.
function perTradeRealized(trades: Trade[]): TradePnl[] {
  const out: TradePnl[] = [];
  const asc = [...trades].sort((a, b) => a.ts_ns - b.ts_ns);
  let pos = 0;
  let avg = 0;
  for (const t of asc) {
    const signed = t.side > 0 ? t.qty : -t.qty;
    let realized = -t.fee;
    if (pos !== 0 && Math.sign(signed) !== Math.sign(pos)) {
      const closing = Math.min(Math.abs(pos), t.qty);
      const dir = pos > 0 ? 1 : -1;
      realized += dir * (t.price - avg) * closing;
      const remaining = Math.abs(pos) - closing;
      if (remaining === 0) {
        const leftover = t.qty - closing;
        if (leftover > 0) {
          pos = (signed > 0 ? 1 : -1) * leftover;
          avg = t.price;
        } else {
          pos = 0;
          avg = 0;
        }
      } else {
        pos = dir * remaining;
      }
    } else {
      const newAbsPos = Math.abs(pos) + t.qty;
      avg = newAbsPos > 0 ? (avg * Math.abs(pos) + t.price * t.qty) / newAbsPos : 0;
      pos += signed;
    }
    out.push({ trade: t, realized });
  }
  return out;
}

export default function SessionHistory() {
  const [trades, setTrades] = useState<Trade[]>([]);
  const [daily, setDaily] = useState<DailyPnl[]>([]);

  useEffect(() => {
    api.trades(500).then(setTrades).catch(() => {});
    api.daily().then(setDaily).catch(() => {});
  }, []);

  const summary = useMemo<DaySummary[]>(() => {
    const dailyMap = new Map<string, DailyPnl>();
    for (const d of daily) dailyMap.set(d.date, d);

    const tps = perTradeRealized(trades);
    const byDay = new Map<string, DaySummary>();
    for (const tp of tps) {
      const key = utcDayKey(tp.trade.ts_ns);
      let day = byDay.get(key);
      if (!day) {
        day = {
          date: key,
          count: 0,
          netPnl: dailyMap.get(key)?.total ?? 0,
          fees: 0,
        };
        byDay.set(key, day);
      }
      day.count++;
      day.fees += tp.trade.fee;
      if (!day.best || tp.realized > day.best.realized) day.best = tp;
      if (!day.worst || tp.realized < day.worst.realized) day.worst = tp;
    }
    return Array.from(byDay.values()).sort((a, b) => b.date.localeCompare(a.date));
  }, [trades, daily]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Session History</h1>
        <span className="page-count">{summary.length} days</span>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th>Date</th>
                  <th className="col-qty">Trades</th>
                  <th className="col-qty">Net P&amp;L</th>
                  <th className="col-qty">Fees</th>
                  <th className="col-qty">Best Trade</th>
                  <th className="col-qty">Worst Trade</th>
                </tr>
              </thead>
              <tbody>
                {summary.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={6}>no sessions</td>
                  </tr>
                ) : (
                  summary.map((d) => (
                    <tr key={d.date}>
                      <td>{d.date}</td>
                      <td className="col-qty">{d.count}</td>
                      <td className={`col-qty ${d.netPnl > 0 ? 'sev-info' : ''}`} style={{ color: d.netPnl > 0 ? 'var(--green)' : d.netPnl < 0 ? 'var(--red)' : undefined }}>
                        {signedMoney(d.netPnl)}
                      </td>
                      <td className="col-qty">{plainMoney(d.fees)}</td>
                      <td className="col-qty" style={{ color: 'var(--green)' }}>
                        {d.best ? `${signedMoney(d.best.realized)} @ ${fmtTimeNs(d.best.trade.ts_ns)}` : '—'}
                      </td>
                      <td className="col-qty" style={{ color: 'var(--red)' }}>
                        {d.worst ? `${signedMoney(d.worst.realized)} @ ${fmtTimeNs(d.worst.trade.ts_ns)}` : '—'}
                      </td>
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
