// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

import React from 'react';

interface Quote {
  price: number;
  qty: number;
}

interface Props {
  bid: Quote | null;
  ask: Quote | null;
  formatPrice: (n: number) => string;
  formatQty: (n: number) => string;
}

export function QuoteBar({ bid, ask, formatPrice, formatQty }: Props) {
  return (
    <div className="quotebar">
      <div className="quote-side quote-bid">
        <div className="quote-left">
          <span className="quote-label bid">BID</span>
          <span className="quote-price bid">{bid ? formatPrice(bid.price) : '—'}</span>
        </div>
        <span className="quote-qty">{bid ? formatQty(bid.qty) : ''}</span>
      </div>
      <div className="quote-side quote-ask">
        <div className="quote-left">
          <span className="quote-label ask">ASK</span>
          <span className="quote-price ask">{ask ? formatPrice(ask.price) : '—'}</span>
        </div>
        <span className="quote-qty">{ask ? formatQty(ask.qty) : ''}</span>
      </div>
    </div>
  );
}
