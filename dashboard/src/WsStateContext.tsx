// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import { createContext, useContext } from 'react';
import type { WsState } from './api';

export interface WsStateCtx {
  wsState: WsState;
  setWsState: (s: WsState) => void;
}

export const WsStateContext = createContext<WsStateCtx>({
  wsState: 'disconnected',
  setWsState: () => {},
});

export function useWsState(): WsStateCtx {
  return useContext(WsStateContext);
}
