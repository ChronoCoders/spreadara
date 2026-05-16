import React, { useEffect, useState } from 'react';
import type { WsState } from '../api';

function formatUTC(d: Date): string {
  const h = String(d.getUTCHours()).padStart(2, '0');
  const m = String(d.getUTCMinutes()).padStart(2, '0');
  const s = String(d.getUTCSeconds()).padStart(2, '0');
  return `${h}:${m}:${s} UTC`;
}

export function Topbar({ wsState }: { wsState: WsState }) {
  const [now, setNow] = useState<Date>(new Date());

  useEffect(() => {
    const id = setInterval(() => setNow(new Date()), 1000);
    return () => clearInterval(id);
  }, []);

  return (
    <header className="topbar">
      <div className="brand">
        <span className="brand-primary">SPREAD</span>
        <span className="brand-accent">ARA</span>
      </div>
      <div className="topbar-right">
        <div className="status-group">
          <span className={`dot ${wsState === 'connected' ? 'dot-green' : 'dot-red'}`} />
          <span>WS</span>
        </div>
        <div className="status-group">
          <span className="dot dot-amber" />
          <span>REST</span>
        </div>
        <div className="clock">{formatUTC(now)}</div>
      </div>
    </header>
  );
}
