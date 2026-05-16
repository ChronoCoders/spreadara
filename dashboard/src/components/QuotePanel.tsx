import { Snapshot } from '../api';

export default function QuotePanel({ snap }: { snap: Snapshot | null }) {
  return (
    <div className="panel">
      <h2>mid price</h2>
      <div className="value">{snap ? snap.mid_price.toFixed(2) : '—'}</div>
      <div className="sub">avg entry {snap ? snap.avg_entry.toFixed(2) : '—'}</div>
    </div>
  );
}
