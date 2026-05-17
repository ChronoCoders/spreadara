import { useEffect, useState } from 'react';
import { api, type OrdersPayload } from '../api';
import { fmtTimeNs } from './fmt';

export default function Orders() {
  const [data, setData] = useState<OrdersPayload | null>(null);

  useEffect(() => {
    const fetch = () => api.orders(100).then(setData).catch(() => {});
    fetch();
    const id = setInterval(fetch, 3000);
    return () => clearInterval(id);
  }, []);

  const events = data?.events ?? [];
  const openCount = data?.open_count ?? 0;

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Orders</h1>
        <span className="page-count">{events.length} events</span>
      </div>

      <div className="section">
        <div className="row-2">
          <div className="metric">
            <div className="metric-label">Open Orders</div>
            <div className="metric-value">{openCount}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Recent State Changes</div>
            <div className="metric-value">{events.length}</div>
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th className="col-time">Time</th>
                  <th className="col-sev">Source</th>
                  <th className="col-src">State</th>
                  <th>Message</th>
                </tr>
              </thead>
              <tbody>
                {events.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={4}>no order events</td>
                  </tr>
                ) : (
                  events.map((e, i) => {
                    const tone = stateTone(e.code);
                    return (
                      <tr key={`${e.ts_ns}-${i}`}>
                        <td className="col-time">{fmtTimeNs(e.ts_ns)}</td>
                        <td className="col-sev">{e.source}</td>
                        <td className="col-src" style={{ color: tone }}>
                          {e.code}
                        </td>
                        <td className="msg">{e.msg}</td>
                      </tr>
                    );
                  })
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

function stateTone(code: string): string | undefined {
  const c = code.toUpperCase();
  if (c.includes('FILL')) return 'var(--green)';
  if (c.includes('CANCEL') || c.includes('REJECT')) return 'var(--red)';
  if (c.includes('ACK')) return 'var(--amber)';
  return undefined;
}
