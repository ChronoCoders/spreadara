#pragma once

#include "execution/i_rest_client.hpp"
#include "infra/config.hpp"

namespace spreadara::execution {

// TODO Phase 7: native FIX 4.4 over TLS to the exchange's FIX gateway.
//   - Session-level Logon (35=A) with ResetSeqNumFlag (141=Y) on first connect
//   - MsgSeqNum (34) management; persist last-sent / last-recv across reconnects
//   - Heartbeat (35=0) interval per Logon negotiation (typ. 30s)
//   - TestRequest (35=1) on idle, Logout (35=5) on shutdown
//   - SequenceReset (35=4) GapFill on demand
//   - Map application-level NewOrderSingle / OrderCancelRequest /
//     OrderCancelReplaceRequest to the same POD ack types defined in
//     i_rest_client.hpp so the OrderManager swap from REST is mechanical —
//     the OrderManager only depends on the method signatures, not the wire.
//
// Stub: every method logs fix_client_not_implemented and returns {ok=false}.
class FixClient {
public:
    FixClient(const infra::Config& cfg, const Credentials& creds);

    OrderAck place_order(const std::string& side, double qty, double price,
                         bool post_only, const std::string& client_order_id);
    CancelAck cancel_order(const std::string& client_order_id, int64_t exchange_order_id);
    AmendAck amend_order(int64_t exchange_order_id, double new_price, double new_qty,
                         const std::string& side, bool post_only,
                         const std::string& replacement_client_order_id);
    PositionsSnapshot query_positions();
    OpenOrdersSnapshot query_open_orders();

private:
    const infra::Config& cfg_;
    const Credentials& creds_;
};

}
