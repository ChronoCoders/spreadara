import { SystemEvent } from '../api';

export default function CircuitBreakerPanel({ events }: { events: SystemEvent[] }) {
  const recent = events.find((e) => e.source === 'circuit_breaker' && e.severity === 'critical');
  const tripped = !!recent;
  return (
    <div className="panel">
      <h2>circuit breaker</h2>
      <div className="value">
        <span className={`dot ${tripped ? 'red' : 'green'}`} />
        {tripped ? 'HALTED' : 'OK'}
      </div>
      <div className="sub">{tripped ? recent!.code : 'running'}</div>
    </div>
  );
}
