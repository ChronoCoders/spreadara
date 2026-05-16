#pragma once

// WHY: the IRestClient interface is defined in rest_client.hpp alongside the
// shared ack / snapshot POD types it returns (avoids a circular header dep —
// the interface signatures reference those POD types by value). This header
// is the canonical include path called out in the Phase-6 spec; it simply
// re-exports the interface so downstream code can `#include "execution/i_rest_client.hpp"`.

#include "execution/rest_client.hpp"
