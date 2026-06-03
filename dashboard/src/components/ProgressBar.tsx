// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import React from 'react';

type Tone = 'green' | 'red' | 'amber' | 'neutral';

interface Props {
  label?: string;
  value: number;
  max: number;
  tone?: Tone;
  ratioText?: string;
}

export function ProgressBar({ label, value, max, tone = 'neutral', ratioText }: Props) {
  const pct = max > 0 ? Math.min(100, Math.max(0, (Math.abs(value) / max) * 100)) : 0;
  const fillClass = tone === 'neutral' ? '' : tone;
  return (
    <div className="progress">
      {label !== undefined && <span className="progress-label">{label}</span>}
      <div className="progress-track">
        <div className={`progress-fill ${fillClass}`} style={{ width: `${pct}%` }} />
      </div>
      {ratioText !== undefined && <span className="progress-ratio">{ratioText}</span>}
    </div>
  );
}
