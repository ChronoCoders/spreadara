import { Snapshot } from '../api';

export default function PnlPanel({ snap }: { snap: Snapshot | null }) {
  const intraday = (snap?.realized_pnl ?? 0) + (snap?.unrealized_pnl ?? 0);
  const cum = snap?.cum_total ?? 0;
  const cls = intraday >= 0 ? 'pos' : 'neg';
  return (
    <div className="panel">
      <h2>p&amp;l</h2>
      <div className={`value ${cls}`}>{snap ? intraday.toFixed(2) : '—'}</div>
      <div className="sub">
        realized {snap ? snap.realized_pnl.toFixed(2) : '—'} ·
        unreal {snap ? snap.unrealized_pnl.toFixed(2) : '—'} ·
        cum {cum.toFixed(2)}
      </div>
    </div>
  );
}
