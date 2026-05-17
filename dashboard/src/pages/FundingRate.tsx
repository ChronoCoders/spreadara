import { useEffect, useState } from 'react';
import { api, type FundingRate, type Snapshot } from '../api';
import { plainMoney, priceFmt } from './fmt';

export default function FundingRatePage() {
  const [fr, setFr] = useState<FundingRate | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [now, setNow] = useState<number>(Date.now());

  useEffect(() => {
    const fetchAll = () => {
      api
        .fundingRate()
        .then((v) => {
          setFr(v);
          setErr(null);
        })
        .catch((e) => setErr(String(e)));
      api.snapshot().then(setSnap).catch(() => {});
    };
    fetchAll();
    const id = setInterval(fetchAll, 30_000);
    const tick = setInterval(() => setNow(Date.now()), 1000);
    return () => {
      clearInterval(id);
      clearInterval(tick);
    };
  }, []);

  const rate = fr?.funding_rate ?? 0;
  const rate8h = fr?.funding_rate_8h ?? rate;
  const nextMs = fr?.next_funding_time ?? 0;
  const countdown = nextMs > 0 ? nextMs - now : 0;

  // 8h expected cost on current inventory: inventory * mid * rate.
  // Positive rate + long position = you pay. Negative rate + long = you earn.
  const inv = snap?.inventory ?? 0;
  const mid = snap?.mid_price ?? 0;
  const expected8h = inv * mid * rate8h;

  const rateIsNeg = rate < 0;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Funding Rate</h1>
        <span className="page-count">OKX BTC-USDT-SWAP · cached 30s</span>
      </div>

      {err && (
        <div className="section">
          <div className="metric">
            <div className="metric-label">Upstream Error</div>
            <div className="metric-value red">{err}</div>
          </div>
        </div>
      )}

      <div className="section">
        <div className="row-4">
          <div className="metric">
            <div className="metric-label">Current Rate</div>
            <div
              className="metric-value"
              style={{ color: rate === 0 ? undefined : rateIsNeg ? 'var(--green)' : 'var(--red)' }}
            >
              {fmtPct(rate)}
            </div>
            <div className="metric-sub">{rateIsNeg ? 'you earn' : 'you pay'} (if long)</div>
          </div>
          <div className="metric">
            <div className="metric-label">8h Rate</div>
            <div className="metric-value">{fmtPct(rate8h)}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Next Funding</div>
            <div className="metric-value">{fmtCountdown(countdown)}</div>
            <div className="metric-sub">{nextMs > 0 ? new Date(nextMs).toISOString().slice(11, 19) + ' UTC' : '—'}</div>
          </div>
          <div className="metric">
            <div className="metric-label">8h Expected Cost</div>
            <div
              className="metric-value"
              style={{ color: expected8h === 0 ? undefined : expected8h > 0 ? 'var(--red)' : 'var(--green)' }}
            >
              {plainMoney(Math.abs(expected8h))}
            </div>
            <div className="metric-sub">
              inv {(inv ?? 0).toFixed(4)} · mid {mid > 0 ? priceFmt.format(mid) : '—'}
            </div>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>NOTES</span></div>
          <div style={{ padding: 16, fontFamily: 'var(--font-sans)', fontSize: 12, color: 'var(--text-secondary)' }}>
            Funding rate is charged every 8 hours. A positive rate means longs pay shorts; a negative
            rate means shorts pay longs. Expected cost above is computed at the current inventory and
            mid price; actual settlement uses the mark price at funding time.
          </div>
        </div>
      </div>
    </div>
  );
}

function fmtPct(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return `${(n * 100).toFixed(4)}%`;
}

function fmtCountdown(ms: number): string {
  if (ms <= 0) return '—';
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return `${pad(h)}:${pad(m)}:${pad(s)}`;
}

function pad(n: number): string {
  return String(n).padStart(2, '0');
}
