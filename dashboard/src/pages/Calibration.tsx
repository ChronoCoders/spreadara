import { useEffect, useState } from 'react';
import { api, type CalibrationRow } from '../api';

export default function Calibration() {
  const [rows, setRows] = useState<CalibrationRow[]>([]);
  const [status, setStatus] = useState<string>('');
  const [busy, setBusy] = useState<boolean>(false);

  const refresh = () =>
    api
      .calibration()
      .then(setRows)
      .catch(() => {});

  useEffect(() => {
    refresh();
  }, []);

  const run = async () => {
    setBusy(true);
    setStatus('');
    try {
      const r = await api.calibrationRun();
      setStatus(`spawn: ${r.status}`);
    } catch (e) {
      setStatus(`error: ${e}`);
    } finally {
      setBusy(false);
    }
  };

  const sorted = [...rows].sort((a, b) => b.sharpe - a.sharpe);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Calibration</h1>
        <div className="page-toolbar">
          <button className="filter-btn" onClick={refresh}>Refresh</button>
          <button className="filter-btn filter-btn-active" disabled={busy} onClick={run}>
            {busy ? 'Starting…' : 'Run Calibration'}
          </button>
          {status && <span className="page-count">{status}</span>}
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header">
            <span>TOP COMBINATIONS</span>
            <span className="count">{sorted.length}</span>
          </div>
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th>γ</th>
                  <th>k</th>
                  <th>T</th>
                  <th className="col-qty">Sharpe</th>
                  <th className="col-qty">P&amp;L</th>
                  <th className="col-qty">Max DD</th>
                  <th className="col-qty">Fills</th>
                </tr>
              </thead>
              <tbody>
                {sorted.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={7}>no calibration results yet — click Run Calibration</td>
                  </tr>
                ) : (
                  sorted.map((row, i) => (
                    <tr key={i} style={i === 0 ? { color: 'var(--amber)' } : undefined}>
                      <td>{row.gamma.toFixed(4)}</td>
                      <td>{row.k.toFixed(3)}</td>
                      <td>{row.t.toFixed(3)}</td>
                      <td className="col-qty">{row.sharpe.toFixed(3)}</td>
                      <td className="col-qty">{row.pnl.toFixed(2)}</td>
                      <td className="col-qty">{(row.max_dd * 100).toFixed(2)}%</td>
                      <td className="col-qty">{row.fills}</td>
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
