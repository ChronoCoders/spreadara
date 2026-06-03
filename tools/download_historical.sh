#!/usr/bin/env bash
# Copyright (c) 2026 ChronoCoders. All rights reserved.
# Proprietary and confidential. Unauthorized copying or distribution is prohibited.

# WHY: bulk-download Binance USDT-M futures bookTicker daily archives for the
# symbol declared in config/config.toml. No hardcoded symbol — comes from cfg.
set -euo pipefail

N="${1:-30}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CFG="${REPO_ROOT}/config/config.toml"
OUT_DIR="${REPO_ROOT}/data/historical"
mkdir -p "${OUT_DIR}"

# Pull `symbol = "..."` from the [market_data] section without a TOML parser.
SYMBOL="$(awk '
    /^\[/ { in_md = ($0 == "[market_data]") ? 1 : 0; next }
    in_md && /^symbol[[:space:]]*=/ {
        gsub(/[",[:space:]]/, "", $0)
        split($0, a, "=")
        print a[2]
        exit
    }
' "${CFG}")"

if [[ -z "${SYMBOL}" ]]; then
    echo "could not extract symbol from ${CFG}" >&2
    exit 1
fi

echo "downloading ${N} days for SYMBOL=${SYMBOL} -> ${OUT_DIR}"

for i in $(seq 1 "${N}"); do
    DATE="$(date -u -d "${i} days ago" +%Y-%m-%d)"
    FILE="${SYMBOL}-bookTicker-${DATE}.zip"
    CSV="${SYMBOL}-bookTicker-${DATE}.csv"
    URL="https://data.binance.vision/data/futures/um/daily/bookTicker/${SYMBOL}/${FILE}"
    if [[ -f "${OUT_DIR}/${CSV}" ]]; then
        echo "skip exists ${CSV}"
        continue
    fi
    echo "GET ${URL}"
    curl -fsSL -o "${OUT_DIR}/${FILE}" "${URL}"
    (cd "${OUT_DIR}" && unzip -o "${FILE}" >/dev/null)
    rm -f "${OUT_DIR}/${FILE}"
done
echo "done"
