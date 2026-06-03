// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

const API_BASE_AUTH = (import.meta as any).env?.VITE_API_BASE ?? '';

interface TokenStore {
  accessToken: string;
  refreshToken: string;
}

let store: TokenStore | null = null;

export interface AuthUser {
  id: number;
  email: string;
  role: string;
}

let currentUser: AuthUser | null = null;

export function getAccessToken(): string | null {
  return store?.accessToken ?? null;
}

export function getCurrentUser(): AuthUser | null {
  return currentUser;
}

export function setTokens(access: string, refresh: string, user?: AuthUser): void {
  store = { accessToken: access, refreshToken: refresh };
  if (user) currentUser = user;
}

export function clearTokens(): void {
  store = null;
  currentUser = null;
}

export function isAuthenticated(): boolean {
  return store !== null;
}

let refreshPromise: Promise<boolean> | null = null;

export async function refreshTokens(): Promise<boolean> {
  if (refreshPromise) return refreshPromise;
  if (!store?.refreshToken) return false;

  refreshPromise = (async () => {
    try {
      const r = await fetch(`${API_BASE_AUTH}/api/auth/refresh`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ refresh_token: store!.refreshToken }),
      });
      if (!r.ok) {
        clearTokens();
        return false;
      }
      const data = await r.json() as { access_token: string; refresh_token: string };
      store = { accessToken: data.access_token, refreshToken: data.refresh_token };
      return true;
    } catch {
      clearTokens();
      return false;
    } finally {
      refreshPromise = null;
    }
  })();

  return refreshPromise;
}

export async function logout(redirectTo = '/login'): Promise<void> {
  if (store?.refreshToken) {
    try {
      await fetch(`${API_BASE_AUTH}/api/auth/logout`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ refresh_token: store.refreshToken }),
      });
    } catch { /* best-effort */ }
  }
  clearTokens();
  window.location.href = redirectTo;
}

// Parsed JWT payload (no signature verification — server does that).
export function parseJwtPayload(token: string): Record<string, unknown> | null {
  try {
    const parts = token.split('.');
    if (parts.length !== 3) return null;
    const payload = atob(parts[1].replace(/-/g, '+').replace(/_/g, '/'));
    return JSON.parse(payload);
  } catch {
    return null;
  }
}

export function tokenExpiresSoon(token: string, bufferSeconds = 60): boolean {
  const payload = parseJwtPayload(token);
  if (!payload || typeof payload.exp !== 'number') return true;
  return payload.exp - bufferSeconds < Date.now() / 1000;
}
