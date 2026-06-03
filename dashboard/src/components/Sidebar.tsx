// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useState, type ReactNode } from 'react';
import { NavLink } from 'react-router-dom';
import { getCurrentUser, logout } from '../auth';

interface NavItem {
  to: string;
  label: string;
  icon: ReactNode;
}

const ICON = {
  size: 16,
  stroke: 1.5,
};

const i = (path: ReactNode) => (
  <svg
    width={ICON.size}
    height={ICON.size}
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    strokeWidth={ICON.stroke}
    strokeLinecap="round"
    strokeLinejoin="round"
    aria-hidden="true"
  >
    {path}
  </svg>
);

const ICONS: Record<string, ReactNode> = {
  home: i(<><path d="M3 11l9-8 9 8" /><path d="M5 10v10h14V10" /></>),
  sliders: i(<><line x1="4" y1="7" x2="20" y2="7" /><line x1="4" y1="12" x2="20" y2="12" /><line x1="4" y1="17" x2="20" y2="17" /><circle cx="9" cy="7" r="2" /><circle cx="15" cy="12" r="2" /><circle cx="8" cy="17" r="2" /></>),
  chart: i(<><line x1="4" y1="20" x2="20" y2="20" /><rect x="6" y="12" width="3" height="6" /><rect x="11" y="8" width="3" height="10" /><rect x="16" y="4" width="3" height="14" /></>),
  trendingUp: i(<><polyline points="3 17 9 11 13 15 21 7" /><polyline points="15 7 21 7 21 13" /></>),
  list: i(<><line x1="8" y1="6" x2="20" y2="6" /><line x1="8" y1="12" x2="20" y2="12" /><line x1="8" y1="18" x2="20" y2="18" /><circle cx="4" cy="6" r="1" /><circle cx="4" cy="12" r="1" /><circle cx="4" cy="18" r="1" /></>),
  clipboard: i(<><rect x="6" y="4" width="12" height="17" rx="1" /><rect x="9" y="2" width="6" height="4" rx="1" /></>),
  play: i(<polygon points="6 4 20 12 6 20 6 4" />),
  grid: i(<><rect x="3" y="3" width="7" height="7" /><rect x="14" y="3" width="7" height="7" /><rect x="3" y="14" width="7" height="7" /><rect x="14" y="14" width="7" height="7" /></>),
  shield: i(<path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6l8-3z" />),
  book: i(<><path d="M5 4h11a2 2 0 0 1 2 2v14H7a2 2 0 0 1-2-2V4z" /><path d="M5 4v14" /></>),
  percent: i(<><line x1="6" y1="18" x2="18" y2="6" /><circle cx="7" cy="7" r="2" /><circle cx="17" cy="17" r="2" /></>),
  calculator: i(<><rect x="5" y="3" width="14" height="18" rx="1" /><rect x="7" y="5" width="10" height="4" /><circle cx="9" cy="13" r="0.5" /><circle cx="12" cy="13" r="0.5" /><circle cx="15" cy="13" r="0.5" /><circle cx="9" cy="17" r="0.5" /><circle cx="12" cy="17" r="0.5" /><circle cx="15" cy="17" r="0.5" /></>),
  archive: i(<><rect x="3" y="4" width="18" height="4" /><path d="M5 8v12h14V8" /><line x1="10" y1="12" x2="14" y2="12" /></>),
  activity: i(<polyline points="3 12 7 12 10 4 14 20 17 12 21 12" />),
  clock: i(<><circle cx="12" cy="12" r="9" /><polyline points="12 7 12 12 16 14" /></>),
  bell: i(<><path d="M6 16V11a6 6 0 0 1 12 0v5l2 2H4l2-2z" /><path d="M10 20a2 2 0 0 0 4 0" /></>),
  terminal: i(<><polyline points="5 8 9 12 5 16" /><line x1="12" y1="16" x2="19" y2="16" /></>),
  wrench: i(<path d="M14.5 3a4 4 0 0 0-3.9 5L3 15.6 5.4 18l7.6-7.6a4 4 0 0 0 5-3.9 4 4 0 0 0-.4-1.7l-2.4 2.4-2-.6-.6-2 2.4-2.4A4 4 0 0 0 14.5 3z" />),
  server: i(<><rect x="3" y="4" width="18" height="6" rx="1" /><rect x="3" y="14" width="18" height="6" rx="1" /><circle cx="7" cy="7" r="0.5" /><circle cx="7" cy="17" r="0.5" /></>),
  users: i(<><path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2" /><circle cx="9" cy="7" r="4" /><path d="M23 21v-2a4 4 0 0 0-3-3.87" /><path d="M16 3.13a4 4 0 0 1 0 7.75" /></>),
  logout: i(<><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4" /><polyline points="16 17 21 12 16 7" /><line x1="21" y1="12" x2="9" y2="12" /></>),
};

const NAV: NavItem[] = [
  { to: '/', label: 'Dashboard', icon: ICONS.home },
  { to: '/strategy', label: 'Strategy', icon: ICONS.sliders },
  { to: '/positions', label: 'Positions', icon: ICONS.chart },
  { to: '/pnl', label: 'P&L', icon: ICONS.trendingUp },
  { to: '/trades', label: 'Trades', icon: ICONS.list },
  { to: '/orders', label: 'Orders', icon: ICONS.clipboard },
  { to: '/backtest', label: 'Backtest', icon: ICONS.play },
  { to: '/calibration', label: 'Calibration', icon: ICONS.grid },
  { to: '/risk', label: 'Risk', icon: ICONS.shield },
  { to: '/market-data', label: 'Market Data', icon: ICONS.book },
  { to: '/funding-rate', label: 'Funding Rate', icon: ICONS.percent },
  { to: '/fee-analysis', label: 'Fee Analysis', icon: ICONS.calculator },
  { to: '/inventory-history', label: 'Inventory History', icon: ICONS.archive },
  { to: '/spread-analysis', label: 'Spread Analysis', icon: ICONS.activity },
  { to: '/session-history', label: 'Session History', icon: ICONS.clock },
  { to: '/alerts', label: 'Alerts', icon: ICONS.bell },
  { to: '/logs', label: 'Logs', icon: ICONS.terminal },
  { to: '/config', label: 'Config', icon: ICONS.wrench },
  { to: '/system', label: 'System', icon: ICONS.server },
];

export function Sidebar() {
  const [open, setOpen] = useState(false);
  const user = getCurrentUser();
  const isAdmin = user?.role === 'admin';

  return (
    <>
      <button
        className="sidebar-toggle"
        aria-label="Toggle navigation"
        onClick={() => setOpen((v) => !v)}
      >
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <line x1="4" y1="7" x2="20" y2="7" />
          <line x1="4" y1="12" x2="20" y2="12" />
          <line x1="4" y1="17" x2="20" y2="17" />
        </svg>
      </button>

      <aside className={`sidebar ${open ? 'sidebar-open' : ''}`}>
        <div className="sidebar-brand">
          <span style={{ fontFamily: 'JetBrains Mono, monospace', fontSize: '14px', fontWeight: 700, letterSpacing: '0.05em' }}>
            SPREAD<span style={{ color: '#f59e0b' }}>ARA</span>
          </span>
        </div>

        <nav className="sidebar-nav">
          {NAV.map((n) => (
            <NavLink
              key={n.to}
              to={n.to}
              end={n.to === '/'}
              className={({ isActive }) =>
                `sidebar-link ${isActive ? 'sidebar-link-active' : ''}`
              }
              onClick={() => setOpen(false)}
            >
              <span className="sidebar-icon">{n.icon}</span>
              <span className="sidebar-label">{n.label}</span>
            </NavLink>
          ))}
          {isAdmin && (
            <NavLink
              to="/admin/users"
              className={({ isActive }) =>
                `sidebar-link ${isActive ? 'sidebar-link-active' : ''}`
              }
              onClick={() => setOpen(false)}
            >
              <span className="sidebar-icon">{ICONS.users}</span>
              <span className="sidebar-label">Users</span>
            </NavLink>
          )}
        </nav>

        <div style={{ marginTop: 'auto', padding: '8px 0' }}>
          {user && (
            <div style={{ padding: '6px 16px', fontSize: '11px', color: 'rgba(255,255,255,0.3)', letterSpacing: '0.04em', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
              {user.email}
            </div>
          )}
          <button
            onClick={() => logout('/login')}
            className="sidebar-link"
            style={{ width: '100%', background: 'none', border: 'none', cursor: 'pointer', textAlign: 'left' }}
          >
            <span className="sidebar-icon">{ICONS.logout}</span>
            <span className="sidebar-label">Sign out</span>
          </button>
        </div>

        <div className="sidebar-version">v0.12.1</div>
      </aside>
    </>
  );
}
