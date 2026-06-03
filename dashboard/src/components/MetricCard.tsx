// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import React from 'react';

interface Props {
  label: string;
  children: React.ReactNode;
}

export function MetricCard({ label, children }: Props) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      {children}
    </div>
  );
}
