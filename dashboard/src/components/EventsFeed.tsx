import React from 'react';
import type { SystemEvent } from '../api';

function parseTs(ts: number | undefined): Date | null {
  if (typeof ts !== 'number' || !Number.isFinite(ts) || ts <= 0) return null;
  const ms = ts > 1e14 ? ts / 1e6 : ts;
  const d = new Date(ms);
  return isNaN(d.getTime()) ? null : d;
}

function fmtTime(ts: number | undefined): string {
  const d = parseTs(ts);
  if (!d) return '—';
  const h = String(d.getUTCHours()).padStart(2, '0');
  const m = String(d.getUTCMinutes()).padStart(2, '0');
  const s = String(d.getUTCSeconds()).padStart(2, '0');
  const ms = String(d.getUTCMilliseconds()).padStart(3, '0');
  return `${h}:${m}:${s}.${ms}`;
}

function sevClass(sev: string): string {
  const s = sev.toLowerCase();
  if (s === 'critical' || s === 'error' || s === 'fatal') return 'sev-critical';
  if (s === 'warn' || s === 'warning') return 'sev-warn';
  return 'sev-info';
}

export function EventsFeed({ events }: { events: SystemEvent[] }) {
  return (
    <div className="panel">
      <div className="panel-header">
        <span>System Events</span>
        <span className="count">{events.length}</span>
      </div>
      <div className="panel-body">
        {/* WHY: same table structure as TradesTable so both panels share an
            identical natural height in the empty state and their data rows
            align horizontally on the same baseline. */}
        <table className="table">
          <thead>
            <tr>
              <th className="col-time">Time</th>
              <th className="col-sev">Sev</th>
              <th className="col-src">Source</th>
              <th>Event</th>
            </tr>
          </thead>
          <tbody>
            {events.length === 0 ? (
              <tr className="empty-row">
                <td colSpan={4}>—</td>
              </tr>
            ) : (
              events.map((e, i) => (
                <tr key={i}>
                  <td className="col-time">{fmtTime(e.ts_ns)}</td>
                  <td className="col-sev">
                    <span className={`sev ${sevClass(e.severity)}`}>{e.severity || '—'}</span>
                  </td>
                  <td className="col-src">{e.source || '—'}</td>
                  <td className="msg" title={e.msg}>
                    {e.msg || e.code || '—'}
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
