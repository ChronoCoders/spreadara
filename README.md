# Spreadara

Spreadara is a low-latency, delta-neutral market-making system for crypto perpetual swaps. It runs an Avellaneda–Stoikov quoter against a live order book, executes through an exchange's REST trading API, and reconciles fills via a private user-data WebSocket. The trading engine is written in C++17 for predictable tail latency; a Go service exposes operational state to a React dashboard for real-time monitoring.

## Architecture

The system is three independently deployable processes communicating through PostgreSQL and lock-free shared-memory rings:

- **Trading engine (C++17).** A single binary running multiple pinned threads. The public-market-data WebSocket thread parses depth and trade frames with simdjson and pushes them to an SPSC ring. A tick-processor thread maintains the order book and publishes `MarketSnapshot` and `QuoteUpdate` FlatBuffers to downstream rings. The strategy thread evaluates the Avellaneda–Stoikov reservation price and optimal spread, the risk thread enforces inventory and drawdown limits, and the execution thread submits and cancels orders through the OKX REST endpoint. A private-WebSocket thread streams real-time fills back into the position tracker. A reporter thread drains a batched event ring into PostgreSQL.
- **Dashboard backend (Go).** A net/http server with a manually-implemented RFC 6455 WebSocket. It reads position snapshots, trades, daily P&L, and system events from PostgreSQL and streams enriched snapshots to connected clients. Built with `CGO_ENABLED=0` for portable static deployment.
- **Frontend (React 18 + Vite + TypeScript).** A terminal-aesthetic dashboard showing live inventory, spread, A–S parameters (γ, k, T), fill rate, maker ratio, latency percentiles, and a streaming trade and event feed.

## Strategy

The quoter implements the Avellaneda–Stoikov optimal market-making model. Reservation price is computed as `s − q · γ · σ² · T`, optimal spread as `γ · σ² · T + (2/γ) · ln(1 + γ/k)`. The strategy targets delta neutrality through inventory-skew quoting: as inventory drifts from zero, the reservation price shifts to bias the next fill back toward flat. Volatility is estimated over a rolling window and clamped by configurable floor and baseline values. Inventory is hard-capped by a `max_inventory` limit; breaches above `inventory_skew_threshold_pct` widen the skew, and breaches above `emergency_unwind_pct` trigger market-out flattening.

## Exchange Support

The first-class venue is **OKX** with `BTC-USDT-SWAP` (contract size 0.01 BTC, 0.1 USD tick). Public market data uses the OKX v5 public WebSocket; orders go through the v5 REST endpoint signed with HMAC-SHA256; fills stream over the v5 private user-data WebSocket. Demo trading (paper) is supported via the `x-simulated-trading: 1` header and the `wspap.okx.com` endpoint. The `[exchange]` block in `config.toml` is exchange-agnostic — Binance USDS-M futures was the original target and code paths for it remain intact, so adding additional venues is a matter of implementing the transport and signer layers.

## Key Features

- **Lock-free SPSC ring buffers** between every thread boundary; no mutexes on the hot path.
- **CPU pinning** for the WS, tick-processor, strategy, risk, execution, and private-WS threads, configurable per-core in `[runtime]`.
- **RDTSC-based timestamping** for sub-microsecond latency measurement on the producer side, with `CLOCK_REALTIME` wall-clock anchoring at sink boundaries.
- **Circuit breaker** with float-epsilon-correct inventory checks, drawdown floor to prevent low-equity false triggers, daily-loss limit, max-open-orders limit, consecutive-rejection limit, and unhedged-inventory watchdog.
- **Private user-data WebSocket** for real-time fill events with strand-confined write-queue serialization and a 25 s client-initiated keepalive ping (OKX disconnects idle private streams at 30 s).
- **Backtesting framework.** A `ReplayEngine` deserializes recorded book-ticker archives and drives the same `SignalAggregator → MarketMaker → OrderManager` pipeline used in production, with a `SimulatedRestClient` providing deterministic fills. Replay results can be optionally pushed through `PgReporter` so the dashboard renders backtest sessions identically to live runs.
- **A–S parameter calibration.** A 2×2×2 smoke grid and a 600-combo full grid sweep over (γ, k, horizon), each producing a CSV of Sharpe, P&L, max drawdown, fill count, and maker ratio.
- **Recorder mode.** The trading binary can record raw book-ticker frames to disk via `--record` for later replay.

## Prerequisites

- WSL2 Ubuntu 22.04+ (or native Linux). GCC 13.3 or newer for C++17 with full `-Wall -Wextra -Werror` cleanliness.
- CMake 3.26+, Ninja, OpenSSL, Boost (system, thread), libpqxx, simdjson, spdlog, FlatBuffers.
- Go 1.21+ for the dashboard backend.
- Node.js 20+ and npm for the React dashboard.
- PostgreSQL 14+ reachable via the `SPREADARA_PG_DSN` connection string.

On Ubuntu:

```
sudo apt install build-essential cmake ninja-build libssl-dev libboost-all-dev \
    libpqxx-dev libsimdjson-dev libspdlog-dev libflatbuffers-dev flatbuffers-compiler \
    postgresql golang-go nodejs npm
```

## Build

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

A sanitized build for development:

```
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Asan
cmake --build build-asan
```

Dashboard backend and frontend:

```
(cd dashboard_backend && CGO_ENABLED=0 go build -o dashboard_backend ./...)
(cd dashboard && npm ci && npm run build)
```

## Configuration

All runtime configuration lives in `config/config.toml`. The relevant sections:

- `[exchange]` — venue, symbol, public/private WebSocket URLs, REST base, contract and tick sizes.
- `[strategy]` — A–S parameters (γ, k, horizon), volatility floor and baseline, inventory caps, quote sizing and lifetime.
- `[risk]` — max position, max order size, daily-loss limit, drawdown limit and minimum-equity floor, rate-limit threshold, max consecutive rejections, max unhedged seconds.
- `[runtime]` — per-thread CPU core pinning.
- `[transport]` — ring-buffer capacities for each pipeline stage.
- `[reporter]` — PostgreSQL batch size, flush interval, connection-pool sizing.
- `[testnet]` — paper-trading endpoints (OKX demo). The committed default is `enabled = false`; operators flip locally.

Secrets are passed through environment variables, never via the config file or command line. The trading binary asserts presence at startup and exits cleanly if missing:

- `SPREADARA_API_KEY`, `SPREADARA_API_SECRET`, `SPREADARA_API_PASSPHRASE` — exchange credentials.
- `SPREADARA_PG_DSN` — PostgreSQL connection string for the reporter.
- `SPREADARA_DASHBOARD_PORT` — optional dashboard backend port override.

## Running

Preflight (system clock skew, file descriptor limits, PG reachability, credentials present):

```
tools/preflight_check.sh
```

Flatten any residual OKX demo positions before starting:

```
tools/okx_demo_flatten.sh
```

Live trading:

```
./build/spreadara --config config/config.toml
```

Recorder:

```
./build/spreadara --config config/config.toml --record
```

Backtest against recorded archives in `[backtest].data_dir`:

```
./build/spreadara --config config/config.toml --backtest
```

Calibration sweep:

```
./build/spreadara --config config/config.toml --calibrate
```

Dashboard backend and frontend (development):

```
./dashboard_backend/dashboard_backend
(cd dashboard && npm run dev)
```

## Dashboard

In development the React frontend serves on `http://localhost:5173` and connects to the Go backend at `ws://localhost:8081`. In production, the Go backend serves the prebuilt static assets directly on a single port. The dashboard displays inventory, current bid/ask and spread (basis points), live A–S parameters, fills per 60 s and 10 s, maker ratio, 60 s rolling average spread, p50/p95/p99 quote-to-ack latency, max open orders, drawdown, and a streaming trades-and-events feed.

## Deployment

Production deployment is handled by `deploy/spreadara-deploy.sh`, which:

1. Builds the C++ release binary, the Go backend, and the React bundle.
2. Installs all artifacts into `/opt/spreadara/`.
3. Installs and reloads the two systemd units (`spreadara-trading.service` and `spreadara-dashboard.service`).
4. Restarts both services.

Credentials live in `/opt/spreadara/env` (mode 600), loaded by systemd via `EnvironmentFile=`. This file is provisioned out-of-band and is never written by the deploy script. The trading service runs under a dedicated `spreadara` user with `CAP_SYS_NICE` (for negative nice values), `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, and `ReadWritePaths=/opt/spreadara`. CPU affinity is pinned to cores 0–5. `RestartPreventExitStatus` covers unrecoverable startup failures (missing credentials, config errors, unknown exchange) so systemd does not spin-restart on permanent faults.

## Release History

- **v0.1.0** — Project scaffold, build system, FlatBuffers schemas, SPSC ring.
- **v0.2.0** — Binance public-market-data WebSocket, order book, tick processor.
- **v0.3.0** — Avellaneda–Stoikov quoter, volatility estimator, market-maker strategy.
- **v0.4.0** — Order manager, REST execution path, ack timeout and reconcile loop.
- **v0.5.0** — Risk thread, circuit breaker, inventory and drawdown limits.
- **v0.6.0** — AddressSanitizer build, lock-free correctness audit, integration tests.
- **v0.7.x** — PostgreSQL reporter, batched async flush, dashboard event ring; backtesting framework and replay engine.
- **v0.8.x** — Go dashboard backend, React frontend, calibration grid, OKX adapter alongside Binance.
- **v0.9.0** — OKX private user-data WebSocket, real-time fill events, write-queue serialization.
- **v0.9.1** — Circuit-breaker float-epsilon fix, A–S parameter wiring to dashboard, trade and event timestamp fixes.
- **v0.9.2** — Drawdown equity floor, fill-rate wiring, 60 s rolling average spread, basis-point precision.
- **v0.9.3** — 25 s keepalive ping on the private WebSocket to prevent 30 s idle disconnect.
- **v0.9.4** — Backtest results wired through `PgReporter`; wall-clock timestamp anchoring for replay events.

## License

Business Source License 1.1. See `LICENSE` for terms. Production use requires a commercial license from ChronoCoders; the license converts to Apache 2.0 four years after each release.
