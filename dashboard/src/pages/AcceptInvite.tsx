// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useState, FormEvent } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { setTokens } from '../auth';

const API_BASE = (import.meta as any).env?.VITE_API_BASE ?? '';

function passwordError(p: string): string {
  if (p.length < 8) return 'At least 8 characters required';
  if (!/[A-Z]/.test(p)) return 'At least one uppercase letter required';
  if (!/[0-9]/.test(p)) return 'At least one number required';
  return '';
}

export default function AcceptInvite() {
  const [params] = useSearchParams();
  const token = params.get('token') ?? '';
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const navigate = useNavigate();

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    setError('');
    const pe = passwordError(password);
    if (pe) { setError(pe); return; }
    if (password !== confirm) { setError('Passwords do not match'); return; }
    if (!token) { setError('Missing invite token'); return; }

    setLoading(true);
    try {
      const r = await fetch(`${API_BASE}/api/auth/accept-invite`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token, password }),
      });
      if (!r.ok) {
        const msg = await r.text().catch(() => 'Failed to accept invite');
        setError(msg);
        return;
      }
      const data = await r.json() as { access_token: string; refresh_token: string; user: { id: number; email: string; role: string } };
      setTokens(data.access_token, data.refresh_token, data.user);
      navigate('/');
    } catch {
      setError('Network error, please try again');
    } finally {
      setLoading(false);
    }
  }

  return (
    <div style={styles.root}>
      <div style={styles.card}>
        <div style={styles.brand}>
          <span style={{ color: '#e5e7eb' }}>SPREAD</span>
          <span style={{ color: '#f59e0b' }}>ARA</span>
        </div>
        <h1 style={styles.title}>Set your password</h1>
        <p style={styles.hint}>Min 8 characters · 1 uppercase · 1 number</p>
        <form onSubmit={handleSubmit} style={styles.form}>
          <div style={styles.field}>
            <label htmlFor="invite-password" style={styles.label}>New password</label>
            <input
              id="invite-password"
              name="invite-password"
              type="password"
              autoComplete="new-password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              required
              style={styles.input}
            />
          </div>
          <div style={styles.field}>
            <label htmlFor="invite-confirm" style={styles.label}>Confirm password</label>
            <input
              id="invite-confirm"
              name="invite-confirm"
              type="password"
              autoComplete="new-password"
              value={confirm}
              onChange={(e) => setConfirm(e.target.value)}
              required
              style={styles.input}
            />
          </div>
          {error && <p style={styles.error}>{error}</p>}
          <button type="submit" disabled={loading} style={styles.btn}>
            {loading ? 'Activating…' : 'Activate account'}
          </button>
        </form>
      </div>
    </div>
  );
}

const styles: Record<string, React.CSSProperties> = {
  root: {
    minHeight: '100vh',
    background: '#0a0a0f',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    fontFamily: 'var(--font-mono, "JetBrains Mono", monospace)',
    padding: '24px',
  },
  card: {
    width: '100%',
    maxWidth: '380px',
    background: '#111118',
    border: '1px solid rgba(245,158,11,0.15)',
    borderRadius: '10px',
    padding: '40px 36px',
  },
  brand: {
    fontSize: '20px',
    fontWeight: 700,
    letterSpacing: '0.08em',
    marginBottom: '20px',
  },
  title: {
    color: '#e5e7eb',
    fontSize: '18px',
    fontWeight: 700,
    margin: '0 0 6px',
  },
  hint: {
    color: 'rgba(255,255,255,0.3)',
    fontSize: '11px',
    margin: '0 0 24px',
  },
  form: { display: 'flex', flexDirection: 'column', gap: '16px' },
  field: { display: 'flex', flexDirection: 'column', gap: '6px' },
  label: {
    color: 'rgba(255,255,255,0.5)',
    fontSize: '11px',
    letterSpacing: '0.08em',
    textTransform: 'uppercase',
  },
  input: {
    background: 'rgba(255,255,255,0.05)',
    border: '1px solid rgba(255,255,255,0.12)',
    borderRadius: '6px',
    color: '#e5e7eb',
    fontFamily: 'inherit',
    fontSize: '14px',
    padding: '10px 12px',
    outline: 'none',
  },
  error: { color: '#f43f5e', fontSize: '12px', margin: '0' },
  btn: {
    background: '#f59e0b',
    border: 'none',
    borderRadius: '6px',
    color: '#0a0a0f',
    cursor: 'pointer',
    fontFamily: 'inherit',
    fontSize: '14px',
    fontWeight: 700,
    padding: '11px',
    marginTop: '4px',
  },
};
