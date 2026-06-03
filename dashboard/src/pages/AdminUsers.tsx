// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useState, useEffect } from 'react';
import { getAccessToken } from '../auth';

const API_BASE = (import.meta as any).env?.VITE_API_BASE ?? '';

interface UserEntry {
  id: number;
  email: string;
  role: string;
  status: 'pending' | 'active';
  activated_at?: string;
  created_at: string;
}

type Role = 'admin' | 'trader' | 'viewer';

export default function AdminUsers() {
  const [users, setUsers] = useState<UserEntry[]>([]);
  const [error, setError] = useState('');
  const [showModal, setShowModal] = useState(false);
  const [inviteEmail, setInviteEmail] = useState('');
  const [inviteRole, setInviteRole] = useState<Role>('viewer');
  const [inviteError, setInviteError] = useState('');
  const [inviting, setInviting] = useState(false);

  function authHeaders() {
    return { Authorization: `Bearer ${getAccessToken() ?? ''}`, 'Content-Type': 'application/json' };
  }

  async function load() {
    setError('');
    try {
      const r = await fetch(`${API_BASE}/api/admin/users`, { headers: authHeaders() });
      if (!r.ok) { setError('Failed to load users'); return; }
      setUsers(await r.json());
    } catch {
      setError('Network error');
    }
  }

  useEffect(() => { void load(); }, []);

  async function handleInvite() {
    setInviteError('');
    if (!inviteEmail) { setInviteError('Email required'); return; }
    setInviting(true);
    try {
      const r = await fetch(`${API_BASE}/api/admin/users`, {
        method: 'POST',
        headers: authHeaders(),
        body: JSON.stringify({ email: inviteEmail, role: inviteRole }),
      });
      if (!r.ok) { setInviteError(await r.text()); return; }
      setShowModal(false);
      setInviteEmail('');
      setInviteRole('viewer');
      void load();
    } catch {
      setInviteError('Network error');
    } finally {
      setInviting(false);
    }
  }

  async function handleDelete(id: number) {
    if (!confirm('Delete this user?')) return;
    try {
      await fetch(`${API_BASE}/api/admin/users/${id}`, { method: 'DELETE', headers: authHeaders() });
      void load();
    } catch { /* ignore */ }
  }

  const fmtDate = (s: string) => new Date(s).toLocaleDateString();

  return (
    <div className="page-content">
      <div className="panel">
        <div className="panel-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span>USERS</span>
          <button className="filter-btn filter-btn-active" onClick={() => setShowModal(true)}>
            + Invite User
          </button>
        </div>
        {error && <p style={{ color: 'var(--red)', padding: '12px 16px', margin: 0 }}>{error}</p>}
        <div style={{ overflowX: 'auto' }}>
          <table className="data-table">
            <thead>
              <tr>
                <th>Email</th>
                <th>Role</th>
                <th>Status</th>
                <th>Created</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              {users.length === 0 ? (
                <tr><td colSpan={5} style={{ textAlign: 'center', color: 'var(--text-dim)', padding: '20px' }}>No users</td></tr>
              ) : users.map((u) => (
                <tr key={u.id}>
                  <td>{u.email}</td>
                  <td><span style={roleStyle(u.role)}>{u.role}</span></td>
                  <td><span style={statusStyle(u.status)}>{u.status}</span></td>
                  <td>{fmtDate(u.created_at)}</td>
                  <td>
                    <button
                      onClick={() => handleDelete(u.id)}
                      style={{ background: 'none', border: 'none', color: 'var(--red)', cursor: 'pointer', fontSize: '12px', padding: '2px 6px' }}
                    >
                      Delete
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>

      {showModal && (
        <div style={overlayStyle}>
          <div style={modalStyle}>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '20px' }}>
              <span style={{ color: 'var(--text-primary)', fontWeight: 700, fontSize: '14px' }}>INVITE USER</span>
              <button onClick={() => setShowModal(false)} style={{ background: 'none', border: 'none', color: 'var(--text-dim)', cursor: 'pointer', fontSize: '18px' }}>×</button>
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '14px' }}>
              <div style={{ display: 'flex', flexDirection: 'column', gap: '6px' }}>
                <label htmlFor="invite-user-email" style={labelStyle}>Email</label>
                <input
                  id="invite-user-email"
                  name="invite-user-email"
                  type="email"
                  className="input"
                  value={inviteEmail}
                  onChange={(e) => setInviteEmail(e.target.value)}
                  placeholder="user@example.com"
                />
              </div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: '6px' }}>
                <label htmlFor="invite-user-role" style={labelStyle}>Role</label>
                <select
                  id="invite-user-role"
                  name="invite-user-role"
                  className="input"
                  value={inviteRole}
                  onChange={(e) => setInviteRole(e.target.value as Role)}
                >
                  <option value="viewer">viewer</option>
                  <option value="trader">trader</option>
                  <option value="admin">admin</option>
                </select>
              </div>
              {inviteError && <p style={{ color: 'var(--red)', fontSize: '12px', margin: 0 }}>{inviteError}</p>}
              <button className="filter-btn filter-btn-active" onClick={handleInvite} disabled={inviting} style={{ width: '100%', justifyContent: 'center' }}>
                {inviting ? 'Sending…' : 'Send Invite'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function roleStyle(role: string): React.CSSProperties {
  const colors: Record<string, string> = { admin: '#f59e0b', trader: '#10b981', viewer: '#6b7280' };
  return { color: colors[role] ?? '#6b7280', fontWeight: 600 };
}

function statusStyle(status: string): React.CSSProperties {
  return { color: status === 'active' ? '#10b981' : '#f59e0b' };
}

const overlayStyle: React.CSSProperties = {
  position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.7)',
  display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000,
};
const modalStyle: React.CSSProperties = {
  background: 'var(--bg-panel)', border: '1px solid var(--border)',
  borderRadius: '8px', padding: '24px', width: '100%', maxWidth: '360px',
};
const labelStyle: React.CSSProperties = {
  color: 'var(--text-dim)', fontSize: '11px', letterSpacing: '0.08em', textTransform: 'uppercase',
};
