import { SystemEvent } from '../api';

// TODO(phase 5+): no latency source wired yet — render placeholder until
// system_events with code=latency are produced.
export default function LatencyPanel({ events }: { events: SystemEvent[] }) {
  const lat = events.find((e) => e.code === 'latency');
  return (
    <div className="panel">
      <h2>latency p50/p95/p99</h2>
      <div className="value">{lat ? lat.msg : '—'}</div>
      <div className="sub">microseconds</div>
    </div>
  );
}
