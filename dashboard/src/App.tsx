import { useEffect, useState } from 'react';
import { api, WebSocketClient, Snapshot, Trade, SystemEvent } from './api';
import QuotePanel from './components/QuotePanel';
import InventoryPanel from './components/InventoryPanel';
import PnlPanel from './components/PnlPanel';
import CircuitBreakerPanel from './components/CircuitBreakerPanel';
import FillRatePanel from './components/FillRatePanel';
import LatencyPanel from './components/LatencyPanel';
import TradesTable from './components/TradesTable';
import EventsFeed from './components/EventsFeed';

export default function App() {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [events, setEvents] = useState<SystemEvent[]>([]);

  useEffect(() => {
    const ws = new WebSocketClient((s) => setSnap(s));
    ws.connect();
    api.snapshot().then(setSnap).catch(() => {});
    const poll = () => {
      api.trades(20).then(setTrades).catch(() => {});
      api.events(50).then(setEvents).catch(() => {});
    };
    poll();
    const id = setInterval(poll, 2000);
    return () => { clearInterval(id); ws.close(); };
  }, []);

  return (
    <div className="app">
      <h1>spreadara dashboard</h1>
      <div className="grid">
        <QuotePanel snap={snap} />
        <InventoryPanel snap={snap} />
        <PnlPanel snap={snap} />
        <CircuitBreakerPanel events={events} />
        <FillRatePanel trades={trades} />
        <LatencyPanel events={events} />
        <div className="panel span-2">
          <h2>last 20 trades</h2>
          <TradesTable trades={trades} />
        </div>
        <div className="panel span-4">
          <h2>system events</h2>
          <EventsFeed events={events} />
        </div>
      </div>
    </div>
  );
}
