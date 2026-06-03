// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import React from 'react';

type PillTone = 'green' | 'red';

export function Pill({ tone, children }: { tone: PillTone; children: React.ReactNode }) {
  return <span className={`pill pill-${tone}`}>{children}</span>;
}
