import { useEffect, useState } from 'react';
import { api, type SystemStatus } from '../api';

export default function System() {
  const [s, setS] = useState<SystemStatus | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    const fetch = () =>
      api
        .status()
        .then((v) => {
          setS(v);
          setErr(null);
        })
        .catch((e) => setErr(String(e)));
    fetch();
    const id = setInterval(fetch, 3000);
    return () => clearInterval(id);
  }, []);

  const tradingOk = (s?.last_snapshot_age_ms ?? Infinity) < 10_000 && !!s;
  const pgOk = !!s?.pg_connected;
  const wsOk = !!s?.ws_connected;
  const dashOk = !err;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">System</h1>
        <span className="page-count">{s?.exchange ?? '—'}</span>
      </div>

      <div className="section">
        <div className="row-4">
          <HealthCard label="Trading Binary" ok={tradingOk} detail={`snapshot ${(s?.last_snapshot_age_ms ?? 0)} ms ago`} />
          <HealthCard label="PostgreSQL" ok={pgOk} detail={pgOk ? 'connected' : 'disconnected'} />
          <HealthCard label="Market WS" ok={wsOk} detail={wsOk ? 'streaming' : 'silent'} />
          <HealthCard label="Dashboard Backend" ok={dashOk} detail={dashOk ? 'reachable' : err ?? ''} />
        </div>
      </div>

      <div className="section">
        <div className="row-2">
          <div className="metric">
            <div className="metric-label">Uptime</div>
            <div className="metric-value">{fmtUptime(s?.uptime_seconds ?? 0)}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Halted</div>
            <div
              className="metric-value"
              style={{ color: s?.halted ? 'var(--red)' : 'var(--green)' }}
            >
              {s?.halted ? 'YES' : 'NO'}
            </div>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>WS STREAMS</span></div>
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr><th>Stream</th><th>State</th></tr>
              </thead>
              <tbody>
                {s?.ws_streams ? (
                  Object.entries(s.ws_streams).map(([name, state]) => (
                    <tr key={name}>
                      <td>{name}</td>
                      <td style={{ color: state === 'up' ? 'var(--green)' : 'var(--red)' }}>{state}</td>
                    </tr>
                  ))
                ) : (
                  <tr className="empty-row"><td colSpan={2}>no status</td></tr>
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

function HealthCard({ label, ok, detail }: { label: string; ok: boolean; detail: string }) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className="metric-value" style={{ color: ok ? 'var(--green)' : 'var(--red)' }}>
        {ok ? 'OK' : 'DOWN'}
      </div>
      <div className="metric-sub">{detail}</div>
    </div>
  );
}

function fmtUptime(sec: number): string {
  if (!Number.isFinite(sec) || sec <= 0) return '—';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const parts: string[] = [];
  if (d > 0) parts.push(`${d}d`);
  if (h > 0 || d > 0) parts.push(`${h}h`);
  parts.push(`${m}m`);
  return parts.join(' ');
}
