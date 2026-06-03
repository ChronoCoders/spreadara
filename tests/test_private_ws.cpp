// OKX private user-data WS parser tests.
//
// No network. Drives the public `detail::` helpers in okx_private_ws_client
// with hand-built JSON fixtures and asserts the resulting FillInput fields
// match the expected wire-to-internal unit conversion (fillSz contracts →
// BTC qty, abs(fee), etc.).

#include <gtest/gtest.h>

#include <string>

#include "infra/config.hpp"
#include "market_data/okx_private_ws_client.hpp"
#include "risk/position_tracker.hpp"

using namespace spreadara;

namespace {

infra::Config make_test_cfg() {
    infra::Config c{};
    c.exchange.symbol = "BTC-USDT-SWAP";
    c.exchange.contract_size = 0.01;
    return c;
}

}  // namespace

TEST(OkxPrivateWs, ParseFillEventFromKnownJson) {
    const std::string json = R"({"data":[{
        "ordId":"123","clOrdId":"spr1z0","instId":"BTC-USDT-SWAP",
        "side":"buy","fillSz":"1","fillPx":"78150.5","fee":"-0.000078",
        "feeCcy":"USDT","state":"filled","uTime":"1234567890123"
    }]})";

    auto cfg = make_test_cfg();
    cfg.exchange.contract_size = 0.01;

    risk::FillInput out;
    const bool ok = market_data::okx::detail::parse_orders_fill(json, cfg, out);
    ASSERT_TRUE(ok);
    EXPECT_EQ(out.order_id, "spr1z0");
    EXPECT_EQ(out.symbol, "BTC-USDT-SWAP");
    EXPECT_EQ(out.side, +1);
    EXPECT_DOUBLE_EQ(out.price, 78150.5);
    EXPECT_DOUBLE_EQ(out.qty, 0.01);          // 1 contract * 0.01 BTC/contract
    EXPECT_DOUBLE_EQ(out.fee, 0.000078);      // abs(fee)
    EXPECT_EQ(out.fee_asset, "USDT");
    EXPECT_EQ(out.timestamp_ns, 1234567890123ULL * 1'000'000ULL);
}

TEST(OkxPrivateWs, FallsBackToOrdIdWhenClOrdIdEmpty) {
    const std::string json = R"({"data":[{
        "ordId":"9999","clOrdId":"","instId":"BTC-USDT-SWAP",
        "side":"sell","fillSz":"2","fillPx":"80000","fee":"-0.0001",
        "feeCcy":"USDT","state":"partially_filled","uTime":"1700000000000"
    }]})";
    auto cfg = make_test_cfg();
    risk::FillInput out;
    ASSERT_TRUE(market_data::okx::detail::parse_orders_fill(json, cfg, out));
    EXPECT_EQ(out.order_id, "9999");
    EXPECT_EQ(out.side, -1);
    EXPECT_DOUBLE_EQ(out.qty, 0.02);
}

TEST(OkxPrivateWs, SkipsNonFillStates) {
    const std::string json = R"({"data":[{
        "ordId":"123","clOrdId":"spr1z0","instId":"BTC-USDT-SWAP",
        "side":"buy","fillSz":"0","fillPx":"78150.5","fee":"0",
        "feeCcy":"USDT","state":"live","uTime":"1234567890123"
    }]})";
    auto cfg = make_test_cfg();
    risk::FillInput out;
    EXPECT_FALSE(market_data::okx::detail::parse_orders_fill(json, cfg, out));
}

TEST(OkxPrivateWs, SkipsZeroFillSz) {
    // state=filled but fillSz=0 — defensive: don't manufacture a phantom fill.
    const std::string json = R"({"data":[{
        "ordId":"123","clOrdId":"spr1z0","instId":"BTC-USDT-SWAP",
        "side":"buy","fillSz":"0","fillPx":"78150.5","fee":"0",
        "feeCcy":"USDT","state":"filled","uTime":"1234567890123"
    }]})";
    auto cfg = make_test_cfg();
    risk::FillInput out;
    EXPECT_FALSE(market_data::okx::detail::parse_orders_fill(json, cfg, out));
}

TEST(OkxPrivateWs, MultiArgSubscribeFormat) {
    const std::string msg =
        market_data::okx::detail::build_subscribe_message("BTC-USDT-SWAP");
    EXPECT_NE(msg.find("\"op\":\"subscribe\""), std::string::npos);
    EXPECT_NE(msg.find("\"channel\":\"orders\""), std::string::npos);
    EXPECT_NE(msg.find("\"channel\":\"positions\""), std::string::npos);
    EXPECT_NE(msg.find("BTC-USDT-SWAP"), std::string::npos);
    // Single args array — both channels share one subscribe frame so Beast
    // never queues a second async_write.
    EXPECT_EQ(msg.find("\"args\""), msg.rfind("\"args\""));
}
