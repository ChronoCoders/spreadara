// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useState } from 'react';
import { api, type Snapshot } from '../api';
import { bpsFmt } from './fmt';

interface ExtSnap extends Snapshot {
  spread_bps?: number;
  volatility?: number;
}

export default function Strategy() {
  const [snap, setSnap] = useState<ExtSnap | null>(null);

  useEffect(() => {
    const fetch = () => api.snapshot().then((s) => setSnap(s as ExtSnap)).catch(() => {});
    fetch();
    const id = setInterval(fetch, 2000);
    return () => clearInterval(id);
  }, []);

  const gamma = snap?.gamma;
  const k = snap?.k;
  const t = snap?.t;
  const vol = snap?.volatility;
  const spread = snap?.spread_bps;
  const inv = snap?.inventory ?? 0;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Strategy — Avellaneda–Stoikov</h1>
      </div>

      <div className="section">
        <div className="row-4">
          <ParamCard label="γ (gamma)" value={gamma} digits={4} />
          <ParamCard label="k" value={k} digits={3} />
          <ParamCard label="T (horizon)" value={t} digits={2} />
          <ParamCard label="Realized Vol" value={vol} digits={5} />
        </div>
      </div>

      <div className="section">
        <div className="row-2">
          <div className="metric">
            <div className="metric-label">Current Spread</div>
            <div className="metric-value">{spread !== undefined ? `${bpsFmt.format(spread)} bps` : '—'}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Inventory Skew</div>
            <div className="metric-value" style={{ color: inv > 0 ? 'var(--green)' : inv < 0 ? 'var(--red)' : undefined }}>
              {inv.toFixed(4)}
            </div>
            <div className="metric-sub">{inv > 0 ? 'long bias' : inv < 0 ? 'short bias' : 'flat'}</div>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>PARAMETER REFERENCE</span></div>
          <div style={{ padding: 16, fontFamily: 'var(--font-sans)', fontSize: 12, color: 'var(--text-secondary)', lineHeight: 1.6 }}>
            <p>
              <strong style={{ color: 'var(--text-primary)' }}>γ (gamma)</strong> — risk aversion.
              Higher γ skews quotes more aggressively against existing inventory and widens the optimal
              spread. Current {fmt(gamma, 4)} {implyGamma(gamma)}.
            </p>
            <p>
              <strong style={{ color: 'var(--text-primary)' }}>k</strong> — order arrival intensity.
              Higher k implies more frequent counterparty fills, which tightens the optimal spread.
              Current {fmt(k, 3)} {implyK(k)}.
            </p>
            <p>
              <strong style={{ color: 'var(--text-primary)' }}>T (horizon)</strong> — time horizon in
              seconds. Longer T grows the inventory-risk term and widens spread; shorter T tightens it.
              Current {fmt(t, 2)} {implyT(t)}.
            </p>
            <p>
              <strong style={{ color: 'var(--text-primary)' }}>Realized volatility</strong> — rolling
              σ used as the inventory-risk multiplier. Current {fmt(vol, 5)}; clamped by the
              configured floor and baseline to avoid quote spikes during quiet books.
            </p>
          </div>
        </div>
      </div>
    </div>
  );
}

function ParamCard({ label, value, digits }: { label: string; value?: number; digits: number }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className={`metric-value ${value === undefined ? 'muted' : ''}`}>{fmt(value, digits)}</div>
    </div>
  );
}

function fmt(v: number | undefined, d: number): string {
  return v !== undefined && Number.isFinite(v) ? v.toFixed(d) : '—';
}
function implyGamma(g?: number): string {
  if (g === undefined) return '';
  if (g < 0.05) return '— low risk aversion, tight quotes';
  if (g > 0.5) return '— high risk aversion, wide quotes';
  return '— moderate risk aversion';
}
function implyK(k?: number): string {
  if (k === undefined) return '';
  if (k < 0.5) return '— low arrival, wider spread';
  if (k > 5) return '— high arrival, tight spread';
  return '— moderate';
}
function implyT(t?: number): string {
  if (t === undefined) return '';
  if (t < 0.5) return '— short horizon, tight';
  if (t > 5) return '— long horizon, wide';
  return '— typical';
}
