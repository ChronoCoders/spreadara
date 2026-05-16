import { Snapshot } from '../api';

export default function InventoryPanel({ snap }: { snap: Snapshot | null }) {
  const inv = snap?.inventory ?? 0;
  const cls = inv > 0 ? 'pos' : inv < 0 ? 'neg' : '';
  return (
    <div className="panel">
      <h2>inventory (BTC)</h2>
      <div className={`value ${cls}`}>{snap ? inv.toFixed(4) : '—'}</div>
      <div className="sub">signed exposure</div>
    </div>
  );
}
