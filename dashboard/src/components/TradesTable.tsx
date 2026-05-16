import { Trade } from '../api';

export default function TradesTable({ trades }: { trades: Trade[] }) {
  if (trades.length === 0) return <div className="empty">no trades yet</div>;
  return (
    <table>
      <thead>
        <tr><th>ts</th><th>side</th><th>px</th><th>qty</th><th>fee</th></tr>
      </thead>
      <tbody>
        {trades.slice(0, 20).map((t, i) => (
          <tr key={i}>
            <td>{new Date(t.ts).toLocaleTimeString()}</td>
            <td className={t.side > 0 ? 'pos' : 'neg'}>{t.side > 0 ? 'BUY' : 'SELL'}</td>
            <td>{t.price.toFixed(2)}</td>
            <td>{t.qty.toFixed(4)}</td>
            <td>{t.fee.toFixed(4)} {t.fee_asset}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
