#include "strategy/signal_aggregator.hpp"

#include "market_snapshot_generated.h"

namespace spreadara::strategy {

bool SignalAggregator::ingest(const SnapshotMsg& msg) {
    if (msg.size == 0 || msg.size > msg.bytes.size()) return false;
    flatbuffers::Verifier v(msg.bytes.data(), msg.size);
    if (!schemas::VerifyMarketSnapshotBuffer(v)) return false;
    const auto* snap = schemas::GetMarketSnapshot(msg.bytes.data());
    if (!snap) return false;

    const double bid_d = snap->bid_depth_5();
    const double ask_d = snap->ask_depth_5();
    const double sum = bid_d + ask_d;

    signals_.mid = snap->mid_price();
    signals_.realized_vol = snap->volatility();
    signals_.sigma_sq = signals_.realized_vol * signals_.realized_vol;
    signals_.bid_depth_5 = bid_d;
    signals_.ask_depth_5 = ask_d;
    signals_.total_depth = sum;
    signals_.depth_imbalance = (sum > 0.0) ? (bid_d - ask_d) / sum : 0.0;
    signals_.last_trade_price = snap->last_trade_price();
    signals_.last_trade_qty = snap->last_trade_qty();
    signals_.valid = true;
    return true;
}

}
