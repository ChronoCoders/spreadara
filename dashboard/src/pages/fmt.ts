// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

export const moneyFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});
export const priceFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});
export const qtyFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 4,
  maximumFractionDigits: 4,
});
export const bpsFmt = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 3,
});
export const intFmt = new Intl.NumberFormat('en-US');

export function signedMoney(n: number): string {
  if (!Number.isFinite(n)) return '—';
  const sign = n > 0 ? '+' : n < 0 ? '−' : '';
  return `${sign}$${moneyFmt.format(Math.abs(n))}`;
}

export function plainMoney(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return `$${moneyFmt.format(n)}`;
}

export function fmtTimeNs(ts_ns: number | undefined): string {
  if (!ts_ns || !Number.isFinite(ts_ns)) return '—';
  const ms = Math.floor(ts_ns / 1_000_000);
  const d = new Date(ms);
  const hh = String(d.getUTCHours()).padStart(2, '0');
  const mm = String(d.getUTCMinutes()).padStart(2, '0');
  const ss = String(d.getUTCSeconds()).padStart(2, '0');
  return `${hh}:${mm}:${ss}`;
}

export function fmtDateNs(ts_ns: number | undefined): string {
  if (!ts_ns || !Number.isFinite(ts_ns)) return '—';
  const ms = Math.floor(ts_ns / 1_000_000);
  const d = new Date(ms);
  return d.toISOString().slice(0, 10);
}

export function utcDayKey(ts_ns: number): string {
  const d = new Date(Math.floor(ts_ns / 1_000_000));
  return d.toISOString().slice(0, 10);
}

export function quantile(sorted: number[], q: number): number {
  if (sorted.length === 0) return NaN;
  const idx = (sorted.length - 1) * q;
  const lo = Math.floor(idx);
  const hi = Math.ceil(idx);
  if (lo === hi) return sorted[lo];
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (idx - lo);
}

export const COLORS = {
  green: '#22c55e',
  red: '#ef4444',
  amber: '#f59e0b',
  muted: '#6b7280',
  border: '#1e2028',
  text: '#e2e4e9',
  surface: '#0d0f13',
};
