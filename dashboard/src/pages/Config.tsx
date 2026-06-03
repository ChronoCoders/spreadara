// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useState } from 'react';
import { api } from '../api';

export default function Config() {
  const [original, setOriginal] = useState('');
  const [content, setContent] = useState('');
  const [editing, setEditing] = useState(false);
  const [status, setStatus] = useState<{ kind: 'ok' | 'err'; msg: string } | null>(null);

  const load = () => {
    api
      .configGet()
      .then((c) => {
        setOriginal(c);
        setContent(c);
        setStatus(null);
      })
      .catch((e) => setStatus({ kind: 'err', msg: String(e) }));
  };

  useEffect(() => {
    load();
  }, []);

  const dirty = content !== original;

  const save = async () => {
    setStatus(null);
    try {
      await api.configSave(content);
      setOriginal(content);
      setEditing(false);
      setStatus({ kind: 'ok', msg: 'saved · restart trading binary for changes to take effect' });
    } catch (e) {
      setStatus({ kind: 'err', msg: String(e) });
    }
  };

  const reset = () => {
    setContent(original);
    setStatus(null);
  };

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Config</h1>
        <div className="page-toolbar">
          {!editing ? (
            <button className="filter-btn filter-btn-active" onClick={() => setEditing(true)}>Edit</button>
          ) : (
            <>
              <button className="filter-btn filter-btn-active" disabled={!dirty} onClick={save}>Save</button>
              <button className="filter-btn" onClick={reset} disabled={!dirty}>Reset</button>
              <button className="filter-btn" onClick={() => { setEditing(false); reset(); }}>Cancel</button>
            </>
          )}
          <button className="filter-btn" onClick={load}>Reload from disk</button>
        </div>
      </div>

      {status?.kind === 'ok' && !dirty && (
        <div
          style={{
            padding: '8px 16px',
            background: 'var(--amber)',
            color: 'var(--bg-base)',
            fontFamily: 'var(--font-sans)',
            fontSize: 11,
            textTransform: 'uppercase',
            letterSpacing: '0.08em',
          }}
        >
          Saved — restart the trading binary for changes to take effect.
        </div>
      )}

      {status?.kind === 'err' && (
        <div
          style={{
            padding: '6px 16px',
            color: 'var(--red)',
            background: 'var(--bg-surface)',
            fontFamily: 'var(--font-mono)',
            fontSize: 11,
            borderBottom: '1px solid var(--bg-border)',
          }}
        >
          {status.msg}
        </div>
      )}

      <div className="section" style={{ flex: 1 }}>
        <div className="panel" style={{ height: '100%' }}>
          {editing ? (
            <textarea
              value={content}
              onChange={(e) => setContent(e.target.value)}
              spellCheck={false}
              style={{
                flex: 1,
                width: '100%',
                minHeight: 480,
                background: 'var(--bg-base)',
                color: 'var(--text-primary)',
                border: 'none',
                outline: 'none',
                fontFamily: 'var(--font-mono)',
                fontSize: 12,
                padding: 16,
                resize: 'none',
              }}
            />
          ) : (
            <pre
              style={{
                flex: 1,
                margin: 0,
                padding: 16,
                background: 'var(--bg-base)',
                color: 'var(--text-primary)',
                fontFamily: 'var(--font-mono)',
                fontSize: 12,
                lineHeight: 1.5,
                overflow: 'auto',
              }}
            >
              {highlightToml(content)}
            </pre>
          )}
        </div>
      </div>
    </div>
  );
}

function highlightToml(src: string): React.ReactNode {
  const lines = src.split('\n');
  return lines.map((line, i) => {
    const trimmed = line.trimStart();
    if (trimmed.startsWith('#')) {
      return (
        <div key={i} style={{ color: 'var(--text-dim)' }}>{line}</div>
      );
    }
    if (trimmed.startsWith('[')) {
      return (
        <div key={i} style={{ color: 'var(--amber)' }}>{line}</div>
      );
    }
    const eqIdx = line.indexOf('=');
    if (eqIdx === -1) {
      return <div key={i}>{line}</div>;
    }
    const left = line.slice(0, eqIdx);
    const right = line.slice(eqIdx + 1);
    return (
      <div key={i}>
        <span style={{ color: 'var(--text-secondary)' }}>{left}</span>
        <span style={{ color: 'var(--text-primary)' }}>=</span>
        <span style={{ color: 'var(--green)' }}>{right}</span>
      </div>
    );
  });
}
