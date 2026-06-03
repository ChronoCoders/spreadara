// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

// Shared "data freshness" primitives for trade-actionable pages. A page is
// considered STALE when its last successful fetch is older than
// STALE_THRESHOLD_MS or when the most recent fetch returned an error. Stale
// data must be visually distinguished (dimmed) and flagged with a banner so an
// operator never mistakes frozen values for live ones.

import { useCallback, useEffect, useState } from 'react';

// Data older than this (ms since last successful fetch) is treated as stale.
export const STALE_THRESHOLD_MS = 5000;

// Opacity applied to data values when the page is stale or disconnected.
export const STALE_OPACITY = 0.4;

// How often the freshness clock re-evaluates age, in ms.
const FRESHNESS_TICK_MS = 1000;

export interface Freshness {
  // True when data is stale (too old or last fetch errored).
  stale: boolean;
  // Call after a successful fetch to record the moment fresh data arrived.
  markSuccess: () => void;
  // Call when a fetch fails to flag the page as errored/stale.
  markError: () => void;
}

// Tracks the timestamp of the last successful fetch plus an error flag, and
// derives a `stale` boolean that flips on once data ages past the threshold.
export function useFreshness(): Freshness {
  const [lastOk, setLastOk] = useState<number | null>(null);
  const [errored, setErrored] = useState(false);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), FRESHNESS_TICK_MS);
    return () => clearInterval(id);
  }, []);

  const stale =
    errored || lastOk === null || now - lastOk > STALE_THRESHOLD_MS;

  const markSuccess = useCallback(() => {
    setLastOk(Date.now());
    setErrored(false);
  }, []);
  const markError = useCallback(() => setErrored(true), []);

  return { stale, markSuccess, markError };
}

// Banner shown at the top of a page when its data is stale.
export function StaleBanner({ show }: { show: boolean }) {
  if (!show) return null;
  return (
    <div className="stale-banner" role="alert">
      STALE DATA — values may be frozen; last update is older than
      {' '}
      {(STALE_THRESHOLD_MS / 1000).toFixed(0)}s or the feed errored
    </div>
  );
}
