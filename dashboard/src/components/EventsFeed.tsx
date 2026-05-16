import React from 'react';
import type { SystemEvent } from '../api';

function parseTs(ts: string): Date | null {
  if (!ts) return null;
  const n = Number(ts);
  if (Number.isFinite(n) && n > 0) {
    const ms = n > 1e14 ? n / 1e6 : n;
    return new Date(ms);
  }
  const d = new Date(ts);
  return isNaN(d.getTime()) ? null : d;
}

function fmtTime(ts: string): string {
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
        {events.length === 0 ? (
          <div className="empty-row" style={{ padding: '24px 16px', textAlign: 'center' }}>
            <span
              style={{
                color: 'var(--text-dim)',
                fontFamily: 'var(--font-mono)',
                fontSize: 11,
              }}
            >
              —
            </span>
          </div>
        ) : (
          <ul className="events-list">
            {events.map((e, i) => (
              <li className="event" key={i}>
                <span className="ts">{fmtTime(e.ts)}</span>
                <span className={`sev ${sevClass(e.severity)}`}>{e.severity || '—'}</span>
                <span className="src">{e.source || '—'}</span>
                <span className="msg" title={e.msg}>
                  {e.msg || e.code || '—'}
                </span>
              </li>
            ))}
          </ul>
        )}
      </div>
    </div>
  );
}
