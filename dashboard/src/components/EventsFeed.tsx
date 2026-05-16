import { SystemEvent } from '../api';

export default function EventsFeed({ events }: { events: SystemEvent[] }) {
  if (events.length === 0) return <div className="empty">no events yet</div>;
  return (
    <div className="feed">
      <table>
        <thead>
          <tr><th>ts</th><th>sev</th><th>source</th><th>code</th><th>msg</th></tr>
        </thead>
        <tbody>
          {events.slice(0, 50).map((e, i) => (
            <tr key={i}>
              <td>{new Date(e.ts).toLocaleTimeString()}</td>
              <td className={e.severity === 'critical' ? 'neg' : ''}>{e.severity}</td>
              <td>{e.source}</td>
              <td>{e.code}</td>
              <td>{e.msg}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
