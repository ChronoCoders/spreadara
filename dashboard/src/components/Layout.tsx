// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { useState } from 'react';
import { Outlet } from 'react-router-dom';
import type { WsState } from '../api';
import { WsStateContext } from '../WsStateContext';
import { Sidebar } from './Sidebar';
import { Topbar } from './Topbar';

export function Layout() {
  const [wsState, setWsState] = useState<WsState>('disconnected');

  return (
    <WsStateContext.Provider value={{ wsState, setWsState }}>
      <div className="layout">
        <Sidebar />
        <div className="layout-content">
          <Topbar wsState={wsState} />
          <Outlet />
        </div>
      </div>
    </WsStateContext.Provider>
  );
}
