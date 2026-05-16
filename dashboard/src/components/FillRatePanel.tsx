import { Trade } from '../api';

export default function FillRatePanel({ trades }: { trades: Trade[] }) {
  const last10s = trades.filter((t) => Date.now() - Date.parse(t.ts) < 10_000).length;
  return (
    <div className="panel">
      <h2>fills (last 10s)</h2>
      <div className="value">{last10s}</div>
      <div className="sub">total recent {trades.length}</div>
    </div>
  );
}
