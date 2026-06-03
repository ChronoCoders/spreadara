// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useState, useEffect, useRef, FormEvent } from 'react';
import { useNavigate } from 'react-router-dom';
import { setTokens } from '../auth';

const API_BASE = (import.meta as any).env?.VITE_API_BASE ?? '';

// Animated candlestick data — deterministic sequence, loops forever.
const CANDLES = [
  { o: 55, h: 72, l: 48, c: 68 },
  { o: 68, h: 80, l: 60, c: 75 },
  { o: 75, h: 82, l: 65, c: 70 },
  { o: 70, h: 78, l: 58, c: 62 },
  { o: 62, h: 71, l: 50, c: 66 },
  { o: 66, h: 85, l: 63, c: 80 },
  { o: 80, h: 90, l: 72, c: 76 },
  { o: 76, h: 84, l: 68, c: 72 },
  { o: 72, h: 79, l: 60, c: 74 },
  { o: 74, h: 88, l: 70, c: 83 },
];

function CandlestickChart() {
  const [tick, setTick] = useState(0);
  const [offset, setOffset] = useState(0);

  useEffect(() => {
    const id = setInterval(() => {
      setTick((t) => t + 1);
      setOffset((o) => (o + 1) % CANDLES.length);
    }, 900);
    return () => clearInterval(id);
  }, []);

  const W = 400;
  const H = 220;
  const PAD = 24;
  const candleW = 28;
  const gap = 10;
  const count = 8;
  const priceMin = 44;
  const priceMax = 94;
  const scaleY = (v: number) => H - PAD - ((v - priceMin) / (priceMax - priceMin)) * (H - PAD * 2);

  const visible = Array.from({ length: count }, (_, i) => CANDLES[(offset + i) % CANDLES.length]);

  // Build a simple line through close prices for the price trail.
  const pts = visible.map((c, i) => {
    const x = PAD + i * (candleW + gap) + candleW / 2;
    const y = scaleY(c.c);
    return `${x},${y}`;
  }).join(' ');

  return (
    <svg width={W} height={H} viewBox={`0 0 ${W} ${H}`} style={{ maxWidth: '100%' }}>
      {/* Grid lines */}
      {[50, 60, 70, 80, 90].map((v) => (
        <line
          key={v}
          x1={PAD} y1={scaleY(v)} x2={W - PAD} y2={scaleY(v)}
          stroke="rgba(245,158,11,0.12)" strokeWidth="1"
        />
      ))}

      {/* Price trail */}
      <polyline
        points={pts}
        fill="none"
        stroke="rgba(245,158,11,0.4)"
        strokeWidth="1.5"
        strokeDasharray="4 2"
      />

      {/* Candles */}
      {visible.map((c, i) => {
        const x = PAD + i * (candleW + gap);
        const bull = c.c >= c.o;
        const color = bull ? '#10b981' : '#f43f5e';
        const bodyTop = scaleY(Math.max(c.o, c.c));
        const bodyBot = scaleY(Math.min(c.o, c.c));
        const bodyH = Math.max(bodyBot - bodyTop, 2);
        const cx = x + candleW / 2;
        const opacity = i === count - 1 ? 0.4 + 0.6 * ((tick % 3) / 2) : 1;
        return (
          <g key={i} style={{ opacity }}>
            <line x1={cx} y1={scaleY(c.h)} x2={cx} y2={scaleY(c.l)} stroke={color} strokeWidth="1.5" />
            <rect x={x + 2} y={bodyTop} width={candleW - 4} height={bodyH} fill={color} rx="1" />
          </g>
        );
      })}
    </svg>
  );
}

export default function Login() {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const navigate = useNavigate();
  const emailRef = useRef<HTMLInputElement>(null);

  useEffect(() => { emailRef.current?.focus(); }, []);

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    setError('');
    setLoading(true);
    try {
      const r = await fetch(`${API_BASE}/api/auth/login`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ email, password }),
      });
      if (r.status === 401) {
        setError('Invalid email or password');
        return;
      }
      if (!r.ok) {
        setError('Login failed, please try again');
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
      {/* Left panel — branding + animated chart */}
      <div style={styles.left}>
        <div style={styles.brand}>
          <span style={styles.brandSpread}>SPREAD</span>
          <span style={styles.brandAra}>ARA</span>
        </div>
        <p style={styles.tagline}>Algorithmic Market Making</p>
        <div style={styles.chart}>
          <CandlestickChart />
        </div>
        <p style={styles.sub}>BTC-USDT-SWAP · Avellaneda–Stoikov · OKX</p>
      </div>

      {/* Right panel — login form */}
      <div style={styles.right}>
        <div style={styles.card}>
          <h1 style={styles.title}>Sign in</h1>
          <form onSubmit={handleSubmit} style={styles.form}>
            <div style={styles.field}>
              <label htmlFor="login-email" style={styles.label}>Email</label>
              <input
                ref={emailRef}
                id="login-email"
                name="login-email"
                type="email"
                autoComplete="username"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                required
                style={styles.input}
              />
            </div>
            <div style={styles.field}>
              <label htmlFor="login-password" style={styles.label}>Password</label>
              <input
                id="login-password"
                name="login-password"
                type="password"
                autoComplete="current-password"
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                required
                style={styles.input}
              />
            </div>
            {error && <p style={styles.error}>{error}</p>}
            <button type="submit" disabled={loading} style={styles.btn}>
              {loading ? 'Signing in…' : 'Sign in'}
            </button>
          </form>
        </div>
      </div>
    </div>
  );
}

const styles: Record<string, React.CSSProperties> = {
  root: {
    display: 'flex',
    minHeight: '100vh',
    background: '#0a0a0f',
    fontFamily: 'var(--font-mono, "JetBrains Mono", monospace)',
  },
  left: {
    flex: '0 0 60%',
    display: 'flex',
    flexDirection: 'column',
    alignItems: 'center',
    justifyContent: 'center',
    padding: '48px',
    background: 'linear-gradient(135deg, #0a0a0f 0%, #111118 100%)',
    borderRight: '1px solid rgba(245,158,11,0.12)',
  },
  brand: {
    fontSize: '36px',
    fontWeight: 700,
    letterSpacing: '0.08em',
    marginBottom: '8px',
  },
  brandSpread: { color: '#e5e7eb' },
  brandAra: { color: '#f59e0b' },
  tagline: {
    color: 'rgba(245,158,11,0.7)',
    fontSize: '13px',
    letterSpacing: '0.1em',
    textTransform: 'uppercase',
    marginBottom: '40px',
    margin: '0 0 40px',
  },
  chart: {
    width: '100%',
    maxWidth: '420px',
    padding: '16px',
    background: 'rgba(245,158,11,0.04)',
    border: '1px solid rgba(245,158,11,0.1)',
    borderRadius: '8px',
  },
  sub: {
    marginTop: '16px',
    color: 'rgba(255,255,255,0.25)',
    fontSize: '11px',
    letterSpacing: '0.05em',
  },
  right: {
    flex: '0 0 40%',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    padding: '48px',
    background: '#111118',
  },
  card: {
    width: '100%',
    maxWidth: '360px',
  },
  title: {
    color: '#e5e7eb',
    fontSize: '22px',
    fontWeight: 700,
    marginBottom: '28px',
  },
  form: {
    display: 'flex',
    flexDirection: 'column',
    gap: '16px',
  },
  field: {
    display: 'flex',
    flexDirection: 'column',
    gap: '6px',
  },
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
    transition: 'border-color 0.15s',
  },
  error: {
    color: '#f43f5e',
    fontSize: '12px',
    margin: '0',
  },
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
    transition: 'opacity 0.15s',
    marginTop: '4px',
  },
};
