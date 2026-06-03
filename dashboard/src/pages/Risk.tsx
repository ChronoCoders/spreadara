// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useMemo, useState } from 'react';
import { api, type SystemEvent } from '../api';
import { fmtTimeNs, utcDayKey } from './fmt';

type SevFilter = 'all' | 'critical' | 'warn' | 'info';
type SrcFilter = 'all' | 'circuit_breaker' | 'risk' | 'ws';

export default function Risk() {
  const [events, setEvents] = useState<SystemEvent[]>([]);
  const [sev, setSev] = useState<SevFilter>('all');
  const [src, setSrc] = useState<SrcFilter>('all');

  useEffect(() => {
    api.events(1000).then(setEvents).catch(() => {});
  }, []);

  const filtered = useMemo(() => {
    return events.filter((e) => {
      if (sev !== 'all' && e.severity.toLowerCase() !== sev) return false;
      if (src !== 'all' && e.source.toLowerCase() !== src) return false;
      return true;
    });
  }, [events, sev, src]);

  const today = useMemo(() => {
    const todayKey = new Date().toISOString().slice(0, 10);
    let crit = 0;
    let warn = 0;
    for (const e of events) {
      if (utcDayKey(e.ts_ns) !== todayKey) continue;
      const s = e.severity.toLowerCase();
      if (s === 'critical') crit++;
      else if (s === 'warn' || s === 'warning') warn++;
    }
    return { crit, warn };
  }, [events]);

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Risk</h1>
      </div>

      <div className="section">
        <div className="row-2">
          <div className="metric">
            <div className="metric-label">Criticals Today</div>
            <div className={`metric-value ${today.crit > 0 ? 'red' : ''}`}>{today.crit}</div>
          </div>
          <div className="metric">
            <div className="metric-label">Warnings Today</div>
            <div className="metric-value" style={{ color: today.warn > 0 ? 'var(--amber)' : undefined }}>
              {today.warn}
            </div>
          </div>
        </div>
      </div>

      <div className="page-header">
        <div className="page-toolbar">
          <FilterButton label="All" active={sev === 'all'} onClick={() => setSev('all')} />
          <FilterButton label="Critical" active={sev === 'critical'} onClick={() => setSev('critical')} />
          <FilterButton label="Warn" active={sev === 'warn'} onClick={() => setSev('warn')} />
          <FilterButton label="Info" active={sev === 'info'} onClick={() => setSev('info')} />
          <span style={{ width: 12 }} />
          <FilterButton label="All Sources" active={src === 'all'} onClick={() => setSrc('all')} />
          <FilterButton label="CB" active={src === 'circuit_breaker'} onClick={() => setSrc('circuit_breaker')} />
          <FilterButton label="Risk" active={src === 'risk'} onClick={() => setSrc('risk')} />
          <FilterButton label="WS" active={src === 'ws'} onClick={() => setSrc('ws')} />
          <span className="page-count">{filtered.length} events</span>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th className="col-time">Time</th>
                  <th className="col-sev">Severity</th>
                  <th className="col-src">Source</th>
                  <th className="col-src">Code</th>
                  <th>Message</th>
                </tr>
              </thead>
              <tbody>
                {filtered.length === 0 ? (
                  <tr className="empty-row">
                    <td colSpan={5}>no events</td>
                  </tr>
                ) : (
                  filtered.map((e, i) => {
                    const s = e.severity.toLowerCase();
                    const sevClass =
                      s === 'critical' ? 'sev-critical' : s.startsWith('warn') ? 'sev-warn' : 'sev-info';
                    return (
                      <tr key={`${e.ts_ns}-${i}`}>
                        <td className="col-time">{fmtTimeNs(e.ts_ns)}</td>
                        <td className={`col-sev sev ${sevClass}`}>{e.severity.toUpperCase()}</td>
                        <td className="col-src">{e.source}</td>
                        <td className="col-src">{e.code}</td>
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

function FilterButton({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  return (
    <button className={`filter-btn ${active ? 'filter-btn-active' : ''}`} onClick={onClick}>
      {label}
    </button>
  );
}
