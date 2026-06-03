#!/usr/bin/env bash
# Copyright (c) 2026 ChronoCoders. All rights reserved.
# Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#
# okx_demo_flatten.sh
#
# Pre-launch helper: closes any residual OKX *demo* position and cancels all
# open orders on BTC-USDT-SWAP, so the spreadara binary can start cleanly
# without tripping the position_divergence circuit breaker.
#
# WHY: a SIGKILL of spreadara (or a crash during a demo run) leaves
# whatever was on the book at the time — quotes that ACK'd, or fills the
# demo market gave us. On restart, reconcile_now() sees local=0 vs
# exchange≠0 → halt. This script wipes that state via the OKX REST API.
#
# **DEMO ONLY** — sends `x-simulated-trading: 1` on every call. Will NOT
# touch a live account.
#
# Usage:
#   export SPREADARA_OKX_API_KEY=...
#   export SPREADARA_OKX_API_SECRET=...
#   export SPREADARA_OKX_PASSPHRASE=...
#   ./tools/okx_demo_flatten.sh
#
# Typical wrapper:
#   ./tools/okx_demo_flatten.sh && ./build/spreadara --config config/config.toml

set -euo pipefail

: "${SPREADARA_OKX_API_KEY:?SPREADARA_OKX_API_KEY not set}"
: "${SPREADARA_OKX_API_SECRET:?SPREADARA_OKX_API_SECRET not set}"
: "${SPREADARA_OKX_PASSPHRASE:?SPREADARA_OKX_PASSPHRASE not set}"

# Symbol — from config if available, otherwise the BTC-USDT-SWAP default.
INST_ID="${SPREADARA_OKX_INST_ID:-BTC-USDT-SWAP}"
MGN_MODE="${SPREADARA_OKX_MGN_MODE:-cross}"
BASE="https://www.okx.com"

okx() {
    local method="$1" path="$2" body="${3:-}"
    local ts
    ts=$(python3 -c "import datetime as d; n=d.datetime.now(d.timezone.utc); print(n.strftime('%Y-%m-%dT%H:%M:%S.')+f'{n.microsecond//1000:03d}Z')")
    local prehash="${ts}${method}${path}${body}"
    local sig
    sig=$(printf '%s' "$prehash" | openssl dgst -sha256 -hmac "$SPREADARA_OKX_API_SECRET" -binary | base64)
    local args=(-sS --fail-with-body
        -H "OK-ACCESS-KEY: $SPREADARA_OKX_API_KEY"
        -H "OK-ACCESS-SIGN: $sig"
        -H "OK-ACCESS-TIMESTAMP: $ts"
        -H "OK-ACCESS-PASSPHRASE: $SPREADARA_OKX_PASSPHRASE"
        -H "x-simulated-trading: 1")
    if [ "$method" = "POST" ]; then
        curl "${args[@]}" -X POST "${BASE}${path}" \
            -H "Content-Type: application/json" -d "$body"
    else
        curl "${args[@]}" "${BASE}${path}"
    fi
}

echo "==> 1/4 query open positions"
POS_JSON=$(okx GET "/api/v5/account/positions?instId=${INST_ID}")
POS_COUNT=$(echo "$POS_JSON" | python3 -c \
    'import json,sys; print(len([p for p in json.load(sys.stdin).get("data",[]) if float(p.get("pos","0"))!=0]))')
echo "    non-zero positions: $POS_COUNT"

if [ "$POS_COUNT" -gt 0 ]; then
    echo "==> 2/4 close-position"
    okx POST "/api/v5/trade/close-position" \
        "{\"instId\":\"${INST_ID}\",\"mgnMode\":\"${MGN_MODE}\"}" >/dev/null
    echo "    closed"
else
    echo "==> 2/4 close-position skipped (nothing to close)"
fi

echo "==> 3/4 cancel open orders"
ORD_JSON=$(okx GET "/api/v5/trade/orders-pending?instId=${INST_ID}")
ORD_IDS=$(echo "$ORD_JSON" | python3 -c \
    'import json,sys; [print(o["ordId"]) for o in json.load(sys.stdin).get("data",[])]')
ORD_COUNT=$(echo "$ORD_IDS" | grep -cv '^$' || true)
echo "    open orders: $ORD_COUNT"
if [ "$ORD_COUNT" -gt 0 ]; then
    for OID in $ORD_IDS; do
        okx POST "/api/v5/trade/cancel-order" \
            "{\"instId\":\"${INST_ID}\",\"ordId\":\"${OID}\"}" >/dev/null
    done
    echo "    cancelled $ORD_COUNT"
fi

echo "==> 4/4 verify clean state"
sleep 1
POS_AFTER=$(okx GET "/api/v5/account/positions?instId=${INST_ID}" | python3 -c \
    'import json,sys; print(len([p for p in json.load(sys.stdin).get("data",[]) if float(p.get("pos","0"))!=0]))')
ORD_AFTER=$(okx GET "/api/v5/trade/orders-pending?instId=${INST_ID}" | python3 -c \
    'import json,sys; print(len(json.load(sys.stdin).get("data",[])))')

if [ "$POS_AFTER" = "0" ] && [ "$ORD_AFTER" = "0" ]; then
    echo "    OK — 0 positions, 0 open orders, safe to start spreadara"
    exit 0
fi
echo "    FAIL — positions=$POS_AFTER open_orders=$ORD_AFTER"
exit 1
