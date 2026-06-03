// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

export function Placeholder({ title }: { title: string }) {
  return (
    <div className="page-placeholder">
      <h1 className="page-title">{title}</h1>
      <p className="page-muted">Coming soon</p>
    </div>
  );
}
