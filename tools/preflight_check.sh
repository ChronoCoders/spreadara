#!/usr/bin/env bash
# preflight_check.sh — verify the host is ready for `spreadara-deploy.sh`.
# Exit 0 if all PASS, 1 otherwise. Never logs secrets.

set -euo pipefail

FAILS=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1: $2"; FAILS=$((FAILS + 1)); }

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# 1. Env vars present.
for v in SPREADARA_OKX_API_KEY SPREADARA_OKX_API_SECRET SPREADARA_OKX_PASSPHRASE SPREADARA_PG_DSN; do
    if [[ -z "${!v:-}" ]]; then
        fail "env_$v" "MISSING: $v"
    else
        pass "env_$v"
    fi
done

# 2. OKX REST connectivity (public endpoint; no credentials).
TIME_BODY=$(mktemp)
HTTP_CODE=$(curl -sS -o "${TIME_BODY}" -w '%{http_code}' \
    --max-time 10 https://www.okx.com/api/v5/public/time || echo "000")
if [[ "${HTTP_CODE}" == "200" ]] && grep -q '"code":"0"' "${TIME_BODY}"; then
    pass "okx_rest"
else
    fail "okx_rest" "http=${HTTP_CODE} body=$(head -c 120 "${TIME_BODY}")"
fi
rm -f "${TIME_BODY}"

# 3. OKX WebSocket connectivity. Prefer websocat; else Python websockets.
WS_URL="wss://ws.okx.com:8443/ws/v5/public"
WS_MSG='{"op":"subscribe","args":[{"channel":"tickers","instId":"BTC-USDT-SWAP"}]}'
if command -v websocat >/dev/null 2>&1; then
    if echo "${WS_MSG}" | timeout 10 websocat --one-message -n "${WS_URL}" >/dev/null 2>&1; then
        pass "okx_ws"
    else
        fail "okx_ws" "websocat connect/read failed"
    fi
elif command -v python3 >/dev/null 2>&1 && python3 -c 'import websockets' >/dev/null 2>&1; then
    if python3 - <<PY
import asyncio, json, websockets
async def main():
    async with websockets.connect("${WS_URL}") as ws:
        await ws.send(json.dumps({"op":"subscribe","args":[{"channel":"tickers","instId":"BTC-USDT-SWAP"}]}))
        await asyncio.wait_for(ws.recv(), timeout=10)
asyncio.run(main())
PY
    then
        pass "okx_ws"
    else
        fail "okx_ws" "python websockets connect/read failed"
    fi
else
    echo "SKIP okx_ws: neither websocat nor python websockets available"
fi

# 4. Postgres connectivity.
if [[ -n "${SPREADARA_PG_DSN:-}" ]]; then
    if [[ "$(psql "${SPREADARA_PG_DSN}" -c 'SELECT 1' -t -A 2>/dev/null || echo '')" == "1" ]]; then
        pass "postgres"
    else
        fail "postgres" "psql probe returned non-1"
    fi
fi

# 5. Config parses + validates.
BIN="${REPO_ROOT}/build/spreadara"
CFG="${REPO_ROOT}/config/config.toml"
if [[ -x "${BIN}" ]]; then
    if "${BIN}" "${CFG}" --validate-config >/dev/null 2>&1; then
        pass "validate_config"
    else
        fail "validate_config" "spreadara --validate-config exited non-zero"
    fi
else
    fail "validate_config" "binary not built at ${BIN}"
fi

if [[ "${FAILS}" -gt 0 ]]; then
    echo "preflight: ${FAILS} failure(s)"
    exit 1
fi
echo "preflight: all checks passed"
exit 0
