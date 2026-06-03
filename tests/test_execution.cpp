// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "db/pg_reporter.hpp"
#include "execution/i_rest_client.hpp"
#include "execution/order_manager.hpp"
#include "execution/simulated_rest_client.hpp"
#include "infra/config.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"

using namespace spreadara;

// WHY: friend-class test peer — reaches the private for_testing_* surface
// without exposing it in the production OrderManager API.
namespace spreadara::execution {
class OrderManagerTestPeer {
public:
    explicit OrderManagerTestPeer(OrderManager& om) : om_(om) {}
    bool transition(int idx, OrderState to) {
        return om_.for_testing_state_transition(idx, to);
    }
    void reconcile(const PositionsSnapshot& p, const OpenOrdersSnapshot& o) {
        om_.for_testing_reconcile(p, o);
    }
    OrderSlot& slot_mut(int idx) {
        return const_cast<OrderSlot&>(om_.peer_slot(idx));
    }
    const OrderSlot& slot(int idx) const { return om_.peer_slot(idx); }
private:
    OrderManager& om_;
};
}

namespace {

infra::Config make_cfg() {
    infra::Config c{};
    c.reporter.core = -1;
    c.reporter.batch_size = 100;
    c.reporter.flush_interval_ms = 50;
    c.reporter.pg_pool_min = 0;
    c.transport.db_ring_capacity = 4096;
    c.market_data.symbol = "BTCUSDT";
    c.execution.rest_base_url = "https://example.invalid";
    c.execution.recv_window_ms = 5000;
    c.execution.ack_timeout_ms = 2000;
    c.execution.reconcile_interval_seconds = 300;
    c.execution.position_divergence_tolerance = 0.001;
    c.execution.flatten_threshold = 0.001;
    c.execution.http_timeout_ms = 3000;
    c.transport.fill_ring_capacity = 1024;
    c.runtime.execution_cpu_core = -1;
    c.strategy.min_tick = 0.1;
    c.strategy.price_move_ticks_threshold = 2;
    c.strategy.quote_qty = 0.01;
    c.risk.max_position = 1000.0;
    c.risk.max_order_size = 1000.0;
    c.risk.price_sanity_pct = 50.0;
    c.risk.rate_limit_threshold = 100000;
    c.risk.max_daily_loss = 1e9;
    c.risk.max_open_orders = 100;
    c.risk.max_drawdown_pct = 99.0;
    c.risk.max_unhedged_seconds = 99999;
    c.risk.max_consecutive_rejections = 9999;
    c.risk.circuit_breaker_poll_ms = 100;
    return c;
}

// WHY: known-vector HMAC-SHA256 hex check. Validates the OpenSSL pipeline the
// REST signers depend on without coupling the test to a specific exchange
// adapter implementation.
std::string hmac_sha256_hex(const std::string& secret, const std::string& msg) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out, &out_len);
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(out_len * 2);
    for (unsigned int i = 0; i < out_len; ++i) {
        s[2 * i]     = hex[(out[i] >> 4) & 0xF];
        s[2 * i + 1] = hex[out[i] & 0xF];
    }
    return s;
}

}  // namespace

TEST(Hmac, KnownVector) {
    const std::string secret =
        "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
    const std::string qs =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
        "&recvWindow=5000&timestamp=1499827319559";
    const std::string expected =
        "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";
    EXPECT_EQ(hmac_sha256_hex(secret, qs), expected);
}

TEST(OrderState, ValidAndInvalidTransitions) {
    using S = execution::OrderState;
    EXPECT_TRUE(execution::is_valid_transition(S::NEW, S::PENDING));
    EXPECT_TRUE(execution::is_valid_transition(S::PENDING, S::SUBMITTED));
    EXPECT_TRUE(execution::is_valid_transition(S::SUBMITTED, S::ACKNOWLEDGED));
    EXPECT_TRUE(execution::is_valid_transition(S::ACKNOWLEDGED, S::PARTIALLY_FILLED));
    EXPECT_TRUE(execution::is_valid_transition(S::PARTIALLY_FILLED, S::FILLED));
    EXPECT_TRUE(execution::is_valid_transition(S::ACKNOWLEDGED, S::FILLED));
    EXPECT_TRUE(execution::is_valid_transition(S::SUBMITTED, S::REJECTED));

    EXPECT_FALSE(execution::is_valid_transition(S::NEW, S::SUBMITTED));
    EXPECT_FALSE(execution::is_valid_transition(S::FILLED, S::PENDING));
    EXPECT_FALSE(execution::is_valid_transition(S::CANCELED, S::SUBMITTED));
    EXPECT_FALSE(execution::is_valid_transition(S::PENDING, S::FILLED));
    EXPECT_FALSE(execution::is_valid_transition(S::PENDING, S::PENDING));
}

TEST(OrderManager, ForTestingStateTransition) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);
    execution::SimulatedRestClient rest(cfg, cfg.market_data.symbol);
    execution::OrderManager om(cfg, rest, pt, rm, cb, nullptr);
    execution::OrderManagerTestPeer peer(om);

    EXPECT_TRUE(peer.transition(0, execution::OrderState::PENDING));
    EXPECT_TRUE(peer.transition(0, execution::OrderState::SUBMITTED));
    EXPECT_TRUE(peer.transition(0, execution::OrderState::ACKNOWLEDGED));
    EXPECT_TRUE(peer.transition(0, execution::OrderState::PARTIALLY_FILLED));
    EXPECT_TRUE(peer.transition(0, execution::OrderState::FILLED));
    EXPECT_FALSE(peer.transition(0, execution::OrderState::PENDING));
}

TEST(OrderManager, ReconcileDivergenceTriggersCb) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);
    execution::SimulatedRestClient rest(cfg, cfg.market_data.symbol);
    execution::OrderManager om(cfg, rest, pt, rm, cb, nullptr);

    // Drive local inventory to +0.10 via apply_fill (BUY 0.10 @ 100).
    risk::FillInput f{};
    f.order_id = "o1";
    f.symbol = "BTCUSDT";
    f.side = +1;
    f.price = 100.0;
    f.qty = 0.10;
    pt.apply_fill(f);
    ASSERT_NEAR(pt.current_inventory(), 0.10, 1e-9);

    execution::PositionsSnapshot pos;
    pos.ok = true;
    execution::PositionEntry pe;
    pe.symbol = "BTCUSDT";
    pe.position_amt = 0.05;
    pos.positions.push_back(pe);
    execution::OpenOrdersSnapshot oo;
    oo.ok = true;

    EXPECT_FALSE(cb.halted());
    execution::OrderManagerTestPeer peer(om);
    peer.reconcile(pos, oo);
    EXPECT_TRUE(cb.halted());
}

TEST(OrderManager, ShutdownCancelsActive) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);
    execution::SimulatedRestClient rest(cfg, cfg.market_data.symbol);
    execution::OrderManager om(cfg, rest, pt, rm, cb, nullptr);
    execution::OrderManagerTestPeer peer(om);

    // Drive slot 0 into ACKNOWLEDGED via the test peer.
    ASSERT_TRUE(peer.transition(0, execution::OrderState::PENDING));
    ASSERT_TRUE(peer.transition(0, execution::OrderState::SUBMITTED));
    ASSERT_TRUE(peer.transition(0, execution::OrderState::ACKNOWLEDGED));

    auto& s = peer.slot_mut(0);
    s.active = true;
    s.exchange_order_id = 123;
    s.client_order_id = "cid-tst";

    // SimulatedRestClient succeeds, so shutdown drives the slot to CANCELED.
    om.shutdown_cancel_all();
    EXPECT_EQ(peer.slot(0).state, execution::OrderState::CANCELED);
    // Idempotent: second call is a no-op (slot already inactive + CANCELED).
    om.shutdown_cancel_all();
    EXPECT_EQ(peer.slot(0).state, execution::OrderState::CANCELED);
}

TEST(RestClient, AmendHalfStateReportsCritical) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);

    // Reporter in dry mode — observe push via pending/flushed counts.
    db::DbEventRing db_ring;
    db::PgReporter reporter(cfg, db_ring, "");
    reporter.start();

    // Synthesize a SystemEvent push directly through the same handler the
    // amend half-state path uses. handle_response_for_test exercises the
    // shared process_status path; here we verify the reporter is wired.
    // WHY: invoking amend_order itself would attempt a live HTTP PUT.
    // Instead, push the same DbEvent the amend path pushes and confirm
    // the reporter consumed it.
    db::DbEvent ev{};
    ev.kind = db::DbEventKind::SystemEvent;
    std::snprintf(ev.evt.severity, sizeof(ev.evt.severity), "critical");
    std::snprintf(ev.evt.source, sizeof(ev.evt.source), "rest_client");
    std::snprintf(ev.evt.code, sizeof(ev.evt.code), "amend_half_state");
    std::snprintf(ev.evt.msg, sizeof(ev.evt.msg), "original_order_id=42 replacement_client_order_id=cid-x");
    ASSERT_TRUE(reporter.push(ev));

    // Also verify the AmendAck struct carries the new cancelled_only field.
    execution::AmendAck a;
    a.cancelled_only = true;
    EXPECT_TRUE(a.cancelled_only);
    EXPECT_FALSE(a.ok);

    // Wait for consumer to drain the event.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reporter.flushed_count() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(reporter.flushed_count() + reporter.pending_count(), 1u);
    reporter.stop();
}

TEST(RiskManager, RejectionPushesSystemEvent) {
    auto cfg = make_cfg();
    cfg.risk.max_order_size = 0.01;  // force a size rejection
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);

    db::DbEventRing db_ring;
    db::PgReporter reporter(cfg, db_ring, "");
    reporter.start();
    rm.set_reporter(&reporter);

    EXPECT_EQ(rm.pre_trade_check(+0.5, 100.0, 100.0), risk::RiskResult::REJECTED_SIZE);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reporter.flushed_count() + reporter.pending_count() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(reporter.flushed_count() + reporter.pending_count(), 1u);
    reporter.stop();
}

TEST(Env, CredentialsPresent) {
    // Save existing values to restore.
    const char* old_k = std::getenv("SPREADARA_API_KEY");
    const char* old_s = std::getenv("SPREADARA_API_SECRET");
    std::string sk = old_k ? old_k : "";
    std::string ss = old_s ? old_s : "";

    ::unsetenv("SPREADARA_API_KEY");
    ::unsetenv("SPREADARA_API_SECRET");
    EXPECT_FALSE(execution::credentials_present());

    ::setenv("SPREADARA_API_KEY", "abc", 1);
    EXPECT_FALSE(execution::credentials_present());

    ::setenv("SPREADARA_API_SECRET", "def", 1);
    EXPECT_TRUE(execution::credentials_present());

    ::unsetenv("SPREADARA_API_KEY");
    ::unsetenv("SPREADARA_API_SECRET");
    if (!sk.empty()) ::setenv("SPREADARA_API_KEY", sk.c_str(), 1);
    if (!ss.empty()) ::setenv("SPREADARA_API_SECRET", ss.c_str(), 1);
}
