#!/usr/bin/env bash
# spreadara-deploy.sh — production deploy / upgrade.
#
# Build, install and reload the trading binary, dashboard backend, and React
# dashboard. NEVER touches /opt/spreadara/env — that file is created
# out-of-band (chmod 600) and contains API keys + SPREADARA_PG_DSN.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "==> 1/11 Build C++ Release"
# WHY: cmake -B reuses an existing cache wholesale. If the dev has been
# building under Debug or Asan, the cached BUILD_TYPE wins and we'd deploy
# the wrong artifact. Use a Release-only build dir so this script can never
# pick up a stale debug/asan cache.
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release

echo "==> 2/11 Build Go dashboard backend"
(cd dashboard_backend && CGO_ENABLED=0 go build -o dashboard_backend ./...)

echo "==> 3/11 Build React dashboard"
(cd dashboard && npm ci && npm run build)

echo "==> 4/11 Install trading binary"
sudo install -m 755 build/release/spreadara /opt/spreadara/spreadara

echo "==> 5/11 Install dashboard backend"
sudo install -m 755 dashboard_backend/dashboard_backend /opt/spreadara/dashboard_backend

echo "==> 6/11 Install React static assets"
# WHY: rm before cp so removed-from-build files don't linger on disk and
# get served as stale assets.
sudo rm -rf /opt/spreadara/dashboard_dist
sudo cp -r dashboard/dist /opt/spreadara/dashboard_dist

echo "==> 7/11 Install config.toml"
sudo install -m 644 config/config.toml /opt/spreadara/config.toml

echo "==> 8/11 Install systemd units"
sudo install -m 644 deploy/spreadara-trading.service /etc/systemd/system/
sudo install -m 644 deploy/spreadara-dashboard.service /etc/systemd/system/

echo "==> 9/11 systemctl daemon-reload"
sudo systemctl daemon-reload

echo "==> 10/11 Restart services"
sudo systemctl restart spreadara-trading spreadara-dashboard

echo "==> 11/11 Verify is-active"
sleep 2
if ! sudo systemctl is-active spreadara-trading spreadara-dashboard; then
    echo "==> Service(s) not active — dumping recent journal lines"
    sudo journalctl -u spreadara-trading -n 30 --no-pager || true
    sudo journalctl -u spreadara-dashboard -n 30 --no-pager || true
    exit 1
fi
echo "deploy_done"
