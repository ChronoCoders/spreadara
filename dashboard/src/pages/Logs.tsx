import { useEffect, useMemo, useRef, useState } from 'react';
import { api, type LogsResponse } from '../api';

type LevelFilter = 'all' | 'critical' | 'warn' | 'info';

export default function Logs() {
  const [data, setData] = useState<LogsResponse>({ lines: [], total_lines: 0 });
  const [filter, setFilter] = useState<LevelFilter>('all');
  const [auto, setAuto] = useState(true);
  const bodyRef = useRef<HTMLDivElement>(null);

  const refresh = () =>
    api
      .logs(500)
      .then(setData)
      .catch(() => {});

  useEffect(() => {
    refresh();
  }, []);

  useEffect(() => {
    if (!auto) return;
    const id = setInterval(refresh, 5000);
    return () => clearInterval(id);
  }, [auto]);

  const lines = useMemo(() => {
    if (filter === 'all') return data.lines;
    return data.lines.filter((l) => detectLevel(l) === filter);
  }, [data.lines, filter]);

  const scrollToBottom = () => {
    if (!bodyRef.current) return;
    bodyRef.current.scrollTop = bodyRef.current.scrollHeight;
  };

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Logs</h1>
        <div className="page-toolbar">
          <FilterButton label="All" active={filter === 'all'} onClick={() => setFilter('all')} />
          <FilterButton label="Critical" active={filter === 'critical'} onClick={() => setFilter('critical')} />
          <FilterButton label="Warn" active={filter === 'warn'} onClick={() => setFilter('warn')} />
          <FilterButton label="Info" active={filter === 'info'} onClick={() => setFilter('info')} />
          <span style={{ width: 12 }} />
          <button
            className={`filter-btn ${auto ? 'filter-btn-active' : ''}`}
            onClick={() => setAuto((v) => !v)}
          >
            {auto ? 'AUTO 5s' : 'PAUSED'}
          </button>
          <button className="filter-btn" onClick={refresh}>Refresh</button>
          <button className="filter-btn" onClick={scrollToBottom}>↓ Bottom</button>
          <span className="page-count">
            {lines.length} / {data.total_lines}
          </span>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div
            ref={bodyRef}
            className="panel-body"
            style={{
              maxHeight: 'calc(100vh - 120px)',
              fontFamily: 'var(--font-mono)',
              fontSize: 11,
              padding: '8px 12px',
              whiteSpace: 'pre',
            }}
          >
            {lines.length === 0 ? (
              <div style={{ color: 'var(--text-muted)' }}>no log lines</div>
            ) : (
              lines.map((l, i) => (
                <div key={i} style={{ color: levelColor(detectLevel(l)) }}>
                  {l}
                </div>
              ))
            )}
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

function detectLevel(line: string): LevelFilter | 'debug' {
  const l = line.toLowerCase();
  if (l.includes('[critical]') || l.includes('[error]') || l.includes('[err]')) return 'critical';
  if (l.includes('[warning]') || l.includes('[warn]')) return 'warn';
  if (l.includes('[debug]') || l.includes('[trace]')) return 'debug';
  if (l.includes('[info]')) return 'info';
  return 'info';
}

function levelColor(level: LevelFilter | 'debug'): string | undefined {
  switch (level) {
    case 'critical':
      return 'var(--red)';
    case 'warn':
      return 'var(--amber)';
    case 'debug':
      return 'var(--text-dim)';
    case 'info':
      return 'var(--text-primary)';
    default:
      return undefined;
  }
}
