import { useEffect, useState } from 'react';
import { api, type BacktestRow } from '../api';
import { plainMoney, signedMoney } from './fmt';

export default function Backtest() {
  const [rows, setRows] = useState<BacktestRow[]>([]);
  const [status, setStatus] = useState<string>('');
  const [busy, setBusy] = useState<boolean>(false);

  const refresh = () =>
    api
      .backtest()
      .then(setRows)
      .catch(() => {});

  useEffect(() => {
    refresh();
  }, []);

  const run = async () => {
    setBusy(true);
    setStatus('');
    try {
      const r = await api.backtestRun();
      setStatus(`spawn: ${r.status}`);
    } catch (e) {
      setStatus(`error: ${e}`);
    } finally {
      setBusy(false);
    }
  };

  const latest = rows[0];

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Backtest</h1>
        <div className="page-toolbar">
          <button className="filter-btn" onClick={refresh}>Refresh</button>
          <button className="filter-btn filter-btn-active" disabled={busy} onClick={run}>
            {busy ? 'Starting…' : 'Run Backtest'}
          </button>
          {status && <span className="page-count">{status}</span>}
        </div>
      </div>

      {latest && (
        <div className="section">
          <div className="row-4">
            <Card
              label="Total P&L"
              value={signedMoney(latest.total_pnl)}
              tone={latest.total_pnl > 0 ? 'green' : latest.total_pnl < 0 ? 'red' : undefined}
            />
            <Card label="Sharpe" value={latest.sharpe_ratio.toFixed(3)} />
            <Card label="Max DD" value={`${latest.max_drawdown_pct.toFixed(3)}%`} />
            <Card label="Fills" value={String(latest.fill_count)} sub={`maker ${(latest.maker_ratio * 100).toFixed(1)}%`} />
          </div>
        </div>
      )}

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header">
            <span>RESULTS</span>
            <span className="count">{rows.length}</span>
          </div>
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th className="col-qty">Total P&amp;L</th>
                  <th className="col-qty">Sharpe</th>
                  <th className="col-qty">Max DD</th>
                  <th className="col-qty">Fills</th>
                  <th className="col-qty">Maker</th>
                  <th className="col-qty">Avg Spread</th>
                  <th className="col-qty">Initial</th>
                  <th className="col-qty">Final</th>
                </tr>
              </thead>
              <tbody>
                {rows.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={8}>no backtest results yet — click Run Backtest</td>
                  </tr>
                ) : (
                  rows.map((r, i) => (
                    <tr key={i}>
                      <td className="col-qty" style={{ color: r.total_pnl > 0 ? 'var(--green)' : r.total_pnl < 0 ? 'var(--red)' : undefined }}>
                        {signedMoney(r.total_pnl)}
                      </td>
                      <td className="col-qty">{r.sharpe_ratio.toFixed(3)}</td>
                      <td className="col-qty">{r.max_drawdown_pct.toFixed(3)}%</td>
                      <td className="col-qty">{r.fill_count}</td>
                      <td className="col-qty">{(r.maker_ratio * 100).toFixed(1)}%</td>
                      <td className="col-qty">{r.avg_spread_captured_bps.toFixed(3)} bps</td>
                      <td className="col-qty">{plainMoney(r.initial_capital)}</td>
                      <td className="col-qty">{plainMoney(r.final_equity)}</td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <div style={{ padding: '8px 16px', color: 'var(--text-muted)', fontFamily: 'var(--font-mono)', fontSize: 11, borderTop: '1px solid var(--bg-border)' }}>
        Note: the C++ writer currently overwrites <code>backtest_results.csv</code> per run, so only the most recent run appears.
      </div>
    </div>
  );
}

function Card({ label, value, sub, tone }: { label: string; value: string; sub?: string; tone?: 'green' | 'red' }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className={`metric-value ${tone ?? ''}`}>{value}</div>
      {sub && <div className="metric-sub">{sub}</div>}
    </div>
  );
}
