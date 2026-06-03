// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useEffect, useState } from 'react';
import { api, type AlertRule } from '../api';

type RuleType = AlertRule['type'];
type Channel = AlertRule['channel'];

const TYPES: { value: RuleType; label: string }[] = [
  { value: 'drawdown', label: 'Drawdown %' },
  { value: 'inventory', label: 'Inventory |abs|' },
  { value: 'pnl', label: 'Realized P&L' },
  { value: 'circuit_breaker', label: 'Circuit Breaker' },
];

export default function Alerts() {
  const [rules, setRules] = useState<AlertRule[]>([]);
  const [name, setName] = useState('');
  const [type, setType] = useState<RuleType>('drawdown');
  const [threshold, setThreshold] = useState<string>('5');
  const [channel, setChannel] = useState<Channel>('log');
  const [webhookUrl, setWebhookUrl] = useState('');
  const [err, setErr] = useState<string | null>(null);

  const refresh = () => api.alerts().then(setRules).catch(() => {});
  useEffect(() => {
    refresh();
  }, []);

  const add = async () => {
    setErr(null);
    if (!name.trim()) {
      setErr('name required');
      return;
    }
    try {
      await api.alertUpsert({
        id: '',
        name: name.trim(),
        type,
        threshold: Number(threshold),
        channel,
        webhook_url: channel === 'webhook' ? webhookUrl.trim() : undefined,
        enabled: true,
      });
      setName('');
      setWebhookUrl('');
      setThreshold('5');
      refresh();
    } catch (e) {
      setErr(String(e));
    }
  };

  const toggle = async (r: AlertRule) => {
    await api.alertUpsert({ ...r, enabled: !r.enabled });
    refresh();
  };

  const remove = async (r: AlertRule) => {
    await api.alertDelete(r.id);
    refresh();
  };

  return (
    <div className="page">
      <div className="page-header">
        <h1 className="page-title">Alerts</h1>
        <span className="page-count">{rules.length} rules</span>
      </div>

      <div className="section">
        <div className="panel">
          <div className="panel-header"><span>ADD RULE</span></div>
          <div style={{ padding: 16, display: 'flex', flexWrap: 'wrap', gap: 10, alignItems: 'flex-end' }}>
            <Field label="Name">
              <input className="input" value={name} onChange={(e) => setName(e.target.value)} placeholder="dd-5pct" />
            </Field>
            <Field label="Type">
              <select className="input" value={type} onChange={(e) => setType(e.target.value as RuleType)}>
                {TYPES.map((t) => (
                  <option key={t.value} value={t.value}>{t.label}</option>
                ))}
              </select>
            </Field>
            <Field label="Threshold">
              <input
                className="input"
                type="number"
                step="0.01"
                value={threshold}
                onChange={(e) => setThreshold(e.target.value)}
                disabled={type === 'circuit_breaker'}
              />
            </Field>
            <Field label="Channel">
              <select className="input" value={channel} onChange={(e) => setChannel(e.target.value as Channel)}>
                <option value="log">log</option>
                <option value="webhook">webhook</option>
              </select>
            </Field>
            {channel === 'webhook' && (
              <Field label="Webhook URL">
                <input
                  className="input"
                  value={webhookUrl}
                  onChange={(e) => setWebhookUrl(e.target.value)}
                  placeholder="https://hooks.slack.com/..."
                  style={{ minWidth: 320 }}
                />
              </Field>
            )}
            <button className="filter-btn filter-btn-active" onClick={add}>Add</button>
            {err && <span style={{ color: 'var(--red)', fontFamily: 'var(--font-mono)', fontSize: 11 }}>{err}</span>}
          </div>
        </div>
      </div>

      <div className="section" style={{ flex: 1 }}>
        <div className="panel">
          <div className="panel-header"><span>RULES</span></div>
          <div className="panel-body">
            <table className="table">
              <thead>
                <tr>
                  <th>Name</th>
                  <th>Type</th>
                  <th className="col-qty">Threshold</th>
                  <th>Channel</th>
                  <th>Webhook</th>
                  <th className="col-side">Enabled</th>
                  <th className="col-side"></th>
                </tr>
              </thead>
              <tbody>
                {rules.length === 0 ? (
                  <tr className="empty-row"><td colSpan={7}>no rules</td></tr>
                ) : (
                  rules.map((r) => (
                    <tr key={r.id}>
                      <td>{r.name}</td>
                      <td>{r.type}</td>
                      <td className="col-qty">{r.threshold}</td>
                      <td>{r.channel}</td>
                      <td style={{ color: 'var(--text-muted)', maxWidth: 220, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {r.webhook_url ?? '—'}
                      </td>
                      <td className="col-side">
                        <button
                          className={`filter-btn ${r.enabled ? 'filter-btn-active' : ''}`}
                          onClick={() => toggle(r)}
                        >
                          {r.enabled ? 'ON' : 'OFF'}
                        </button>
                      </td>
                      <td className="col-side">
                        <button className="filter-btn" onClick={() => remove(r)} style={{ color: 'var(--red)' }}>
                          ×
                        </button>
                      </td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 4, fontFamily: 'var(--font-sans)', fontSize: 10, color: 'var(--text-secondary)', textTransform: 'uppercase', letterSpacing: '0.08em' }}>
      {label}
      {children}
    </label>
  );
}
