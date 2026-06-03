// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { BrowserRouter, Route, Routes } from 'react-router-dom';
import { Layout } from './components/Layout';
import Dashboard from './pages/Dashboard';
import Strategy from './pages/Strategy';
import Positions from './pages/Positions';
import PnL from './pages/PnL';
import Trades from './pages/Trades';
import Orders from './pages/Orders';
import Backtest from './pages/Backtest';
import Calibration from './pages/Calibration';
import Risk from './pages/Risk';
import MarketData from './pages/MarketData';
import FundingRate from './pages/FundingRate';
import FeeAnalysis from './pages/FeeAnalysis';
import InventoryHistory from './pages/InventoryHistory';
import SpreadAnalysis from './pages/SpreadAnalysis';
import SessionHistory from './pages/SessionHistory';
import Alerts from './pages/Alerts';
import Logs from './pages/Logs';
import Config from './pages/Config';
import System from './pages/System';

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route element={<Layout />}>
          <Route index element={<Dashboard />} />
          <Route path="strategy" element={<Strategy />} />
          <Route path="positions" element={<Positions />} />
          <Route path="pnl" element={<PnL />} />
          <Route path="trades" element={<Trades />} />
          <Route path="orders" element={<Orders />} />
          <Route path="backtest" element={<Backtest />} />
          <Route path="calibration" element={<Calibration />} />
          <Route path="risk" element={<Risk />} />
          <Route path="market-data" element={<MarketData />} />
          <Route path="funding-rate" element={<FundingRate />} />
          <Route path="fee-analysis" element={<FeeAnalysis />} />
          <Route path="inventory-history" element={<InventoryHistory />} />
          <Route path="spread-analysis" element={<SpreadAnalysis />} />
          <Route path="session-history" element={<SessionHistory />} />
          <Route path="alerts" element={<Alerts />} />
          <Route path="logs" element={<Logs />} />
          <Route path="config" element={<Config />} />
          <Route path="system" element={<System />} />
        </Route>
      </Routes>
    </BrowserRouter>
  );
}
