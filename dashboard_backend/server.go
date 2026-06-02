package main

// WHY: dbReader interface lets tests inject a stub without a live Postgres.
// WS implementation: manual RFC 6455 handshake + frame writer using
// net/http's Hijacker (chosen over x/net/websocket so go.sum stays minimal
// with only lib/pq — gorilla/websocket disallowed per spec).

import (
	"bufio"
	"context"
	"crypto/sha1"
	"database/sql"
	"encoding/base64"
	"encoding/binary"
	"encoding/csv"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

// WHY: schema stores ts as nanoseconds-since-epoch BIGINT (TsNs). Clients
// convert to ISO8601 for display; transport stays lossless and avoids
// driver-side timestamp parsing.
type Snapshot struct {
	TsNs       int64   `json:"ts_ns"`
	Inventory  float64 `json:"inventory"`
	AvgEntry   float64 `json:"avg_entry"`
	Realized   float64 `json:"realized_pnl"`
	Unrealized float64 `json:"unrealized_pnl"`
	Fees       float64 `json:"total_fees"`
	Mid        float64 `json:"mid_price"`
	CumTotal   float64 `json:"cum_total"`
	// Phase 8: real values now sourced from extended position_snapshots row.
	BidPrice             float64 `json:"bid_price"`
	AskPrice             float64 `json:"ask_price"`
	SpreadBps            float64 `json:"spread_bps"`
	CircuitBreakerHalted bool    `json:"circuit_breaker_halted"`
	OpenOrders           int     `json:"open_orders"`
	MaxOpenOrders        int     `json:"max_open_orders"`
	CurrentDrawdownPct   float64 `json:"current_drawdown_pct"`
	MaxDrawdownPct       float64 `json:"max_drawdown_pct"`
	// Phase 8 telemetry fields:
	BidQty              float64 `json:"bid_qty"`
	AskQty              float64 `json:"ask_qty"`
	Volatility          float64 `json:"volatility"`
	Gamma               float64 `json:"gamma"`
	K                   float64 `json:"k"`
	T                   float64 `json:"t"`
	LatP50Us            float64 `json:"lat_p50_us"`
	LatP95Us            float64 `json:"lat_p95_us"`
	LatP99Us            float64 `json:"lat_p99_us"`
	MaxInventoryDisplay float64 `json:"max_inventory_display"`
	// Computed in the handler from the trades table, not stored in the snapshot:
	FillCount10s int     `json:"fill_count_10s"`
	FillCount60s int     `json:"fill_count_60s"`
	MakerRatio   float64 `json:"maker_ratio"`
	FillsPer10s  float64 `json:"fills_per_10s"`
	AvgSpreadBps float64 `json:"avg_spread_bps"`
}

type Trade struct {
	TsNs     int64   `json:"ts_ns"`
	OrderID  string  `json:"order_id"`
	Side     int     `json:"side"`
	Price    float64 `json:"price"`
	Qty      float64 `json:"qty"`
	Fee      float64 `json:"fee"`
	FeeAsset string  `json:"fee_asset"`
	IsMaker  bool    `json:"is_maker"`
}

type DailyPnl struct {
	Date       string  `json:"date"`
	Realized   float64 `json:"realized"`
	Unrealized float64 `json:"unrealized"`
	Fees       float64 `json:"fees"`
	Total      float64 `json:"total"`
}

type SystemEvent struct {
	TsNs     int64  `json:"ts_ns"`
	Severity string `json:"severity"`
	Source   string `json:"source"`
	Code     string `json:"code"`
	Msg      string `json:"msg"`
}

type SpreadPoint struct {
	TsNs      int64   `json:"ts_ns"`
	SpreadBps float64 `json:"spread_bps"`
}

type InventoryPoint struct {
	TsNs      int64   `json:"ts_ns"`
	Inventory float64 `json:"inventory"`
	MidPrice  float64 `json:"mid_price"`
}

type OrdersPayload struct {
	OpenCount int           `json:"open_count"`
	Events    []SystemEvent `json:"events"`
}

type FundingRate struct {
	FundingRate     float64 `json:"funding_rate"`
	NextFundingTime int64   `json:"next_funding_time"`
	FundingRate8h   float64 `json:"funding_rate_8h"`
}

type AlertRule struct {
	ID         string  `json:"id"`
	Name       string  `json:"name"`
	Type       string  `json:"type"`
	Threshold  float64 `json:"threshold"`
	Channel    string  `json:"channel"`
	WebhookURL string  `json:"webhook_url,omitempty"`
	Enabled    bool    `json:"enabled"`
}

type LogsResponse struct {
	Lines      []string `json:"lines"`
	TotalLines int      `json:"total_lines"`
}

type BacktestRow struct {
	RunTs                 string  `json:"run_ts"`
	TotalPnl              float64 `json:"total_pnl"`
	SharpeRatio           float64 `json:"sharpe_ratio"`
	MaxDrawdownPct        float64 `json:"max_drawdown_pct"`
	FillCount             int     `json:"fill_count"`
	MakerRatio            float64 `json:"maker_ratio"`
	AvgSpreadCapturedBps  float64 `json:"avg_spread_captured_bps"`
	InitialCapital        float64 `json:"initial_capital"`
	FinalEquity           float64 `json:"final_equity"`
}

type CalibrationRow struct {
	Gamma  float64 `json:"gamma"`
	K      float64 `json:"k"`
	T      float64 `json:"t"`
	Sharpe float64 `json:"sharpe"`
	Pnl    float64 `json:"pnl"`
	MaxDD  float64 `json:"max_dd"`
	Fills  int     `json:"fills"`
}

type dbReader interface {
	latestSnapshot() (Snapshot, error)
	recentTrades(limit int) ([]Trade, error)
	dailyPnl() ([]DailyPnl, error)
	recentEvents(limit int) ([]SystemEvent, error)
	spreadHistory(limit int) ([]SpreadPoint, error)
	inventoryHistory(limit int) ([]InventoryPoint, error)
	orderEvents(limit int) ([]SystemEvent, error)
}

// haltedReader is an optional capability for readers that can compute the
// halted-state / drawdown from the events + snapshots tables. sqlReader
// implements it; the test stubReader does not, so the server falls back to a
// zero/false default in that case.
type haltedReader interface {
	circuitHalted() (bool, error)
	drawdownPct() (float64, error)
	wsStreamsUp() (bool, error)
}

// fillStatsReader is an optional capability for readers that can compute
// fill-rate / maker-ratio stats from the trades table. sqlReader implements
// it; the test stubReader does not, so handlers fall back to zeros.
type fillStatsReader interface {
	fillStats() (int, int, float64, float64, error)
}

// spreadStatsReader is an optional capability for the 60s rolling average
// spread, computed from position_snapshots (the trading binary writes a
// per-snapshot spread_bps at ~1 Hz). Separate from fillStatsReader because
// the source table is different and we want stubs to opt-in independently.
type spreadStatsReader interface {
	avgSpreadBps60s() (float64, error)
}

// statusReader is an optional capability for /api/v5/status. sqlReader
// implements it via lastSnapshotTs + dbPing; test stubs can implement it
// with deliberate-failure variants to exercise the 500 paths.
type statusReader interface {
	lastSnapshotTs() (int64, error)
	dbPing(ctx context.Context) error
}

type sqlReader struct{ db *sql.DB }

func (r *sqlReader) latestSnapshot() (Snapshot, error) {
	var s Snapshot
	// Phase 8: NullFloat64 / NullInt64 for the columns added via ALTER. Rows
	// inserted before Phase 8 carry NULL for these and must scan cleanly.
	var bb, ba, sp, bq, aq, vol, gm, kk, tp, l50, l95, l99 sql.NullFloat64
	var oo sql.NullInt64
	err := r.db.QueryRow(
		`SELECT ts_ns, inventory, avg_entry, realized_pnl, unrealized_pnl, total_fees, mid_price,
		        best_bid, best_ask, spread_bps, bid_qty, ask_qty, volatility, gamma, k, T_param,
		        lat_p50_us, lat_p95_us, lat_p99_us, open_orders
		 FROM position_snapshots ORDER BY ts_ns DESC LIMIT 1`,
	).Scan(&s.TsNs, &s.Inventory, &s.AvgEntry, &s.Realized, &s.Unrealized, &s.Fees, &s.Mid,
		&bb, &ba, &sp, &bq, &aq, &vol, &gm, &kk, &tp, &l50, &l95, &l99, &oo)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return s, err
	}
	s.BidPrice = bb.Float64
	s.AskPrice = ba.Float64
	s.SpreadBps = sp.Float64
	s.BidQty = bq.Float64
	s.AskQty = aq.Float64
	s.Volatility = vol.Float64
	s.Gamma = gm.Float64
	s.K = kk.Float64
	s.T = tp.Float64
	s.LatP50Us = l50.Float64
	s.LatP95Us = l95.Float64
	s.LatP99Us = l99.Float64
	s.OpenOrders = int(oo.Int64)
	_ = r.db.QueryRow(`SELECT COALESCE(SUM(total),0) FROM daily_pnl`).Scan(&s.CumTotal)
	return s, nil
}

// dbPing wraps PingContext so the test stub can implement the statusReader
// interface and inject errors without touching a real *sql.DB.
func (r *sqlReader) dbPing(ctx context.Context) error {
	if r.db == nil {
		return errors.New("nil db")
	}
	return r.db.PingContext(ctx)
}

// lastSnapshotTs returns the most recent position_snapshots.ts_ns or 0 if
// the table is empty. WHY: /api/v5/status only needs the timestamp to
// compute ws_connected + last_snapshot_age_ms; calling latestSnapshot()
// would pull 20 columns and run the cum_total subquery just to discard
// everything except ts_ns.
func (r *sqlReader) lastSnapshotTs() (int64, error) {
	var ts int64
	err := r.db.QueryRow(
		`SELECT ts_ns FROM position_snapshots ORDER BY ts_ns DESC LIMIT 1`,
	).Scan(&ts)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return 0, err
	}
	return ts, nil
}

// fillStats returns (count10s, count60s, makerRatio, fillsPer10s) computed
// from the trades table in a single query. is_maker may be NULL on legacy
// rows — COALESCE to TRUE matches the column default.
func (r *sqlReader) fillStats() (int, int, float64, float64, error) {
	nowNs := time.Now().UnixNano()
	cut10 := nowNs - int64(10)*int64(time.Second)
	cut60 := nowNs - int64(60)*int64(time.Second)
	var f10, f60, makers, total int
	err := r.db.QueryRow(
		`SELECT
		   COUNT(*) FILTER (WHERE ts_ns > $1) AS f10,
		   COUNT(*) FILTER (WHERE ts_ns > $2) AS f60,
		   COUNT(*) FILTER (WHERE COALESCE(is_maker, TRUE)) AS makers,
		   COUNT(*) AS total
		 FROM trades
		 WHERE ts_ns > $2`,
		cut10, cut60,
	).Scan(&f10, &f60, &makers, &total)
	if err != nil {
		return 0, 0, 0, 0, err
	}
	var ratio float64
	if total > 0 {
		ratio = float64(makers) / float64(total)
	}
	return f10, f60, ratio, float64(f10), nil
}

// avgSpreadBps60s returns the mean of position_snapshots.spread_bps over the
// last 60s, ignoring NULLs and rows where spread_bps <= 0 (those represent
// "book not yet ready" snapshots, not real zero spreads). Returns 0 if no
// usable rows in the window.
func (r *sqlReader) avgSpreadBps60s() (float64, error) {
	cutoff := time.Now().Add(-60*time.Second).UnixNano()
	var avg sql.NullFloat64
	err := r.db.QueryRow(
		`SELECT AVG(spread_bps) FROM position_snapshots
		 WHERE ts_ns >= $1 AND spread_bps IS NOT NULL AND spread_bps > 0`,
		cutoff,
	).Scan(&avg)
	if err != nil {
		return 0, err
	}
	return avg.Float64, nil
}

func (r *sqlReader) recentTrades(limit int) ([]Trade, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, order_id, side, price, qty, fee, fee_asset, COALESCE(is_maker, TRUE)
		 FROM trades ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Trade{}
	for rows.Next() {
		var t Trade
		var side int16
		if err := rows.Scan(&t.TsNs, &t.OrderID, &side, &t.Price, &t.Qty, &t.Fee, &t.FeeAsset, &t.IsMaker); err != nil {
			return nil, err
		}
		t.Side = int(side)
		out = append(out, t)
	}
	return out, rows.Err()
}

func (r *sqlReader) dailyPnl() ([]DailyPnl, error) {
	rows, err := r.db.Query(`SELECT date, realized, unrealized, fees, total FROM daily_pnl ORDER BY date DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []DailyPnl{}
	for rows.Next() {
		var d DailyPnl
		var dt time.Time
		if err := rows.Scan(&dt, &d.Realized, &d.Unrealized, &d.Fees, &d.Total); err != nil {
			return nil, err
		}
		d.Date = dt.Format("2006-01-02")
		out = append(out, d)
	}
	return out, rows.Err()
}

// circuitHalted reports true if any of the halt-trigger codes was emitted in
// the last hour. The trading binary does not write a per-state flag, so we
// derive it from system_events here.
func (r *sqlReader) circuitHalted() (bool, error) {
	cutoff := time.Now().Add(-1*time.Hour).UnixNano()
	var n int
	err := r.db.QueryRow(
		`SELECT COUNT(*) FROM system_events
		 WHERE ts_ns >= $1
		   AND code IN ('drawdown','unhedged','consecutive_rejections','ws_disconnect','exception')`,
		cutoff,
	).Scan(&n)
	if err != nil {
		return false, err
	}
	return n > 0, nil
}

// drawdownPct = (peak_equity - current_equity)/peak_equity * 100 over 24h.
// equity is approximated as realized + unrealized - fees from position_snapshots.
func (r *sqlReader) drawdownPct() (float64, error) {
	cutoff := time.Now().Add(-24*time.Hour).UnixNano()
	rows, err := r.db.Query(
		`SELECT realized_pnl + unrealized_pnl - total_fees AS equity
		 FROM position_snapshots WHERE ts_ns >= $1 ORDER BY ts_ns ASC`, cutoff)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	peak := 0.0
	current := 0.0
	first := true
	for rows.Next() {
		var eq float64
		if err := rows.Scan(&eq); err != nil {
			return 0, err
		}
		if first || eq > peak {
			peak = eq
			first = false
		}
		current = eq
	}
	if peak <= 0 {
		return 0, nil
	}
	return (peak - current) / peak * 100.0, nil
}

// wsStreamsUp reports true if a position_snapshots row exists within the last
// 30s — the only liveness proxy available until the trading binary writes
// per-stream health rows.
func (r *sqlReader) wsStreamsUp() (bool, error) {
	cutoff := time.Now().Add(-30*time.Second).UnixNano()
	var n int
	err := r.db.QueryRow(
		`SELECT COUNT(*) FROM position_snapshots WHERE ts_ns >= $1`, cutoff,
	).Scan(&n)
	if err != nil {
		return false, err
	}
	return n > 0, nil
}

// spreadHistory returns the most recent `limit` rows of (ts_ns, spread_bps)
// from position_snapshots in descending ts order. NULL or non-positive
// spread_bps values are filtered (book-not-ready rows).
func (r *sqlReader) spreadHistory(limit int) ([]SpreadPoint, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, spread_bps FROM position_snapshots
		 WHERE spread_bps IS NOT NULL AND spread_bps > 0
		 ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []SpreadPoint{}
	for rows.Next() {
		var p SpreadPoint
		if err := rows.Scan(&p.TsNs, &p.SpreadBps); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// inventoryHistory returns (ts_ns, inventory, mid_price) ordered by ts_ns desc.
func (r *sqlReader) inventoryHistory(limit int) ([]InventoryPoint, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, inventory, mid_price FROM position_snapshots
		 ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []InventoryPoint{}
	for rows.Next() {
		var p InventoryPoint
		if err := rows.Scan(&p.TsNs, &p.Inventory, &p.MidPrice); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// orderEvents returns recent system_events filtered to sources that emit
// order-lifecycle transitions (execution / order manager / private user-data
// WebSocket). Used by /api/orders.
func (r *sqlReader) orderEvents(limit int) ([]SystemEvent, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, severity, source, code, msg FROM system_events
		 WHERE source IN ('execution','order_manager','okx_private_ws')
		 ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []SystemEvent{}
	for rows.Next() {
		var e SystemEvent
		if err := rows.Scan(&e.TsNs, &e.Severity, &e.Source, &e.Code, &e.Msg); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

func (r *sqlReader) recentEvents(limit int) ([]SystemEvent, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, severity, source, code, msg FROM system_events ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []SystemEvent{}
	for rows.Next() {
		var e SystemEvent
		if err := rows.Scan(&e.TsNs, &e.Severity, &e.Source, &e.Code, &e.Msg); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

type server struct {
	r          dbReader
	wsInterval time.Duration
	corsOrigin string
	// Phase 7: configuration surfaced via env vars and merged into responses.
	maxOpenOrders  int
	maxDrawdownPct float64
	exchangeName   string
	startTime      time.Time
	// Phase 8: progress-bar denominator for the inventory gauge. Env override
	// SPREADARA_MAX_INVENTORY_DISPLAY; config-file value is operator-doc only
	// (the Go backend does not parse TOML).
	maxInventoryDisplay float64
	// v0.11 additions: funding rate is fetched live from OKX with a 30s cache,
	// calibration result list is parsed from a CSV on disk, and a calibration
	// run forks the trading binary as a background process. All three are
	// substitutable so tests don't hit the network/disk/exec.
	fundingFetcher    func(context.Context) (FundingRate, error)
	calibrationCsv    string
	calibrationRunner func() error

	fundingMu   sync.Mutex
	fundingAt   time.Time
	fundingVal  FundingRate
	fundingErr  error
	fundingTTL  time.Duration

	// v0.12 additions: alert rules persisted to JSON, log tailing, config
	// read/write. All paths are env-overridable so the tests don't touch
	// production files.
	alerts     *alertStore
	alertFire  alertFirer
	logPath    string
	configPath string

	// v0.12 additions: backtest results CSV + run forker. Mirrors the
	// calibration pattern.
	backtestCsv    string
	backtestRunner func() error
}

func newServer(r dbReader, wsInterval time.Duration, corsOrigin string) *server {
	maxOO := 10
	if v := os.Getenv("SPREADARA_MAX_OPEN_ORDERS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			maxOO = n
		}
	}
	maxDd := 5.0
	if v := os.Getenv("SPREADARA_MAX_DRAWDOWN_PCT"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil && f > 0 {
			maxDd = f
		}
	}
	exch := os.Getenv("SPREADARA_EXCHANGE")
	if exch == "" {
		exch = "okx"
	}
	maxInvDisp := 0.1
	if v := os.Getenv("SPREADARA_MAX_INVENTORY_DISPLAY"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil && f > 0 {
			maxInvDisp = f
		}
	}
	return &server{
		r: r, wsInterval: wsInterval, corsOrigin: corsOrigin,
		maxOpenOrders:       maxOO,
		maxDrawdownPct:      maxDd,
		exchangeName:        exch,
		startTime:           time.Now(),
		maxInventoryDisplay: maxInvDisp,
		fundingFetcher:      fetchOKXFundingRate,
		calibrationCsv:      envStrLocal("SPREADARA_CALIBRATION_CSV", "calibration_top10.csv"),
		calibrationRunner:   defaultCalibrationRunner,
		fundingTTL:          30 * time.Second,
		alerts:              newAlertStore(envStrLocal("SPREADARA_ALERTS_PATH", "alerts.json")),
		alertFire:           defaultAlertFirer,
		logPath:             envStrLocal("SPREADARA_LOG_PATH", "logs/spreadara.log"),
		configPath:          envStrLocal("SPREADARA_CONFIG_PATH", "config/config.toml"),
		backtestCsv:         envStrLocal("SPREADARA_BACKTEST_CSV", "backtest_results.csv"),
		backtestRunner:      defaultBacktestRunner,
	}
}

func defaultBacktestRunner() error {
	return runTradingBinary("--backtest")
}

// readBacktestCSV parses the C++ backtest writer's summary CSV. Current
// writer emits header + a single row per run (overwrites on each run), so
// the result is 0 or 1 entries. Returned as a slice so the API stays
// future-compatible with multi-row history.
func readBacktestCSV(path string) ([]BacktestRow, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	rd := csv.NewReader(f)
	rd.FieldsPerRecord = -1
	records, err := rd.ReadAll()
	if err != nil {
		return nil, err
	}
	if len(records) <= 1 {
		return []BacktestRow{}, nil
	}
	idx := map[string]int{}
	for i, h := range records[0] {
		idx[strings.ToLower(strings.TrimSpace(h))] = i
	}
	get := func(row []string, names ...string) string {
		for _, n := range names {
			if i, ok := idx[n]; ok && i < len(row) {
				return strings.TrimSpace(row[i])
			}
		}
		return ""
	}
	out := make([]BacktestRow, 0, len(records)-1)
	for _, row := range records[1:] {
		if len(row) == 0 {
			continue
		}
		runTs := get(row, "run_ts", "timestamp", "ts")
		pnl, _ := strconv.ParseFloat(get(row, "total_pnl", "pnl"), 64)
		sharpe, _ := strconv.ParseFloat(get(row, "sharpe_ratio", "sharpe"), 64)
		dd, _ := strconv.ParseFloat(get(row, "max_drawdown_pct", "max_dd", "maxdd"), 64)
		fills, _ := strconv.Atoi(get(row, "fill_count", "fills"))
		maker, _ := strconv.ParseFloat(get(row, "maker_ratio"), 64)
		spread, _ := strconv.ParseFloat(get(row, "avg_spread_captured_bps", "avg_spread_bps"), 64)
		init, _ := strconv.ParseFloat(get(row, "initial_capital"), 64)
		final, _ := strconv.ParseFloat(get(row, "final_equity"), 64)
		out = append(out, BacktestRow{
			RunTs: runTs,
			TotalPnl: pnl, SharpeRatio: sharpe, MaxDrawdownPct: dd,
			FillCount: fills, MakerRatio: maker, AvgSpreadCapturedBps: spread,
			InitialCapital: init, FinalEquity: final,
		})
	}
	// Reverse so newest is first — the C++ writer appends, so on disk the
	// rows are oldest-first.
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out, nil
}

func envStrLocal(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

// fetchOKXFundingRate calls OKX's public funding-rate endpoint. instId is
// fixed to BTC-USDT-SWAP per the system's primary symbol; override via env
// var SPREADARA_FUNDING_INST_ID if a future symbol becomes primary.
func fetchOKXFundingRate(ctx context.Context) (FundingRate, error) {
	instId := envStrLocal("SPREADARA_FUNDING_INST_ID", "BTC-USDT-SWAP")
	url := "https://www.okx.com/api/v5/public/funding-rate?instId=" + instId
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return FundingRate{}, err
	}
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return FundingRate{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return FundingRate{}, errors.New("okx http " + strconv.Itoa(resp.StatusCode))
	}
	var body struct {
		Code string `json:"code"`
		Data []struct {
			FundingRate     string `json:"fundingRate"`
			NextFundingTime string `json:"nextFundingTime"`
		} `json:"data"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return FundingRate{}, err
	}
	if body.Code != "0" || len(body.Data) == 0 {
		return FundingRate{}, errors.New("okx response code=" + body.Code)
	}
	fr, _ := strconv.ParseFloat(body.Data[0].FundingRate, 64)
	nt, _ := strconv.ParseInt(body.Data[0].NextFundingTime, 10, 64)
	return FundingRate{
		FundingRate:     fr,
		NextFundingTime: nt,
		FundingRate8h:   fr, // OKX rate is already the 8-hour funding rate.
	}, nil
}

// runTradingBinary forks the trading binary with the given flags. The
// binary expects to find config/config.toml and data/ relative to its CWD,
// so the spawn is rooted at SPREADARA_HOME (default: ".") and the CSV/log
// outputs land there. stdout/stderr go to a tmp file so a crash leaves a
// trail without polluting the dashboard's own log.
func runTradingBinary(args ...string) error {
	bin := envStrLocal("SPREADARA_BIN", "./spreadara")
	home := envStrLocal("SPREADARA_HOME", ".")
	cmd := exec.Command(bin, args...)
	cmd.Dir = home
	logPath := envStrLocal("SPREADARA_SPAWN_LOG", "/tmp/spreadara-spawn.log")
	var logFile *os.File
	if f, err := os.OpenFile(logPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644); err == nil {
		cmd.Stdout = f
		cmd.Stderr = f
		logFile = f
	}
	if err := cmd.Start(); err != nil {
		if logFile != nil {
			_ = logFile.Close()
		}
		return err
	}
	// WHY: reap the spawned child so it doesn't linger as a zombie once it
	// exits — the dashboard never blocks on it, so Wait runs in its own
	// goroutine. Closing the spawn-log fd here avoids leaking one descriptor
	// per spawn.
	go func() {
		_ = cmd.Wait()
		if logFile != nil {
			_ = logFile.Close()
		}
	}()
	return nil
}

func defaultCalibrationRunner() error {
	return runTradingBinary("--calibration-smoke")
}

// enrichSnapshot fills the Phase-7 derived fields on top of whatever the
// dbReader returned. Splits cleanly so unit tests using stubReader (which
// doesn't implement haltedReader) keep returning the base snapshot.
func (s *server) enrichSnapshot(snap *Snapshot) {
	snap.MaxOpenOrders = s.maxOpenOrders
	snap.MaxDrawdownPct = s.maxDrawdownPct
	snap.MaxInventoryDisplay = s.maxInventoryDisplay
	if hr, ok := s.r.(haltedReader); ok {
		if h, err := hr.circuitHalted(); err == nil {
			snap.CircuitBreakerHalted = h
		}
		if d, err := hr.drawdownPct(); err == nil {
			snap.CurrentDrawdownPct = d
		}
	}
	if fr, ok := s.r.(fillStatsReader); ok {
		if f10, f60, ratio, fp10, err := fr.fillStats(); err == nil {
			snap.FillCount10s = f10
			snap.FillCount60s = f60
			snap.MakerRatio = ratio
			snap.FillsPer10s = fp10
		}
	}
	if sr, ok := s.r.(spreadStatsReader); ok {
		if avg, err := sr.avgSpreadBps60s(); err == nil {
			snap.AvgSpreadBps = avg
		}
	}
}

// originAllowed implements the shared origin allowlist used by both CORS and
// the WebSocket upgrade. WHY: CORS does NOT gate WS connections, so /ws must
// run this check explicitly before hijacking — otherwise any page on the
// internet could open a socket to the dashboard.
func (s *server) originAllowed(origin string) bool {
	if origin == "" {
		return false
	}
	if s.corsOrigin != "" && origin == s.corsOrigin {
		return true
	}
	if strings.HasPrefix(origin, "http://localhost:") || strings.HasPrefix(origin, "http://127.0.0.1:") {
		return true
	}
	return false
}

func (s *server) cors(w http.ResponseWriter, r *http.Request) {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return
	}
	if s.originAllowed(origin) {
		w.Header().Set("Access-Control-Allow-Origin", origin)
		w.Header().Set("Access-Control-Allow-Methods", "GET, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
	}
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("json encode error: %v", err)
	}
}

// writeStatusError writes a JSON error body with the given HTTP status.
// WHY: /status surfaces operational health — a 500 with structured body
// tells an operator "this backend is degraded, here's why" rather than
// silently returning stale data with no error indicator.
func writeStatusError(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func parseLimit(r *http.Request, def, cap int) int {
	q := r.URL.Query().Get("limit")
	if q == "" {
		return def
	}
	n, err := strconv.Atoi(q)
	if err != nil || n <= 0 {
		return def
	}
	if n > cap {
		return cap
	}
	return n
}

func (s *server) handleSnapshot(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	snap, err := s.r.latestSnapshot()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	s.enrichSnapshot(&snap)
	if s.alerts != nil && s.alertFire != nil {
		s.alerts.evaluate(snap, s.alertFire)
	}
	writeJSON(w, snap)
}

// handleStatus serves GET /api/v5/status — exchange-agnostic process health.
// WS-stream up/down is a coarse "any snapshot in last 30s" probe because the
// trading binary does not (yet) write per-stream health rows.
func (s *server) handleStatus(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	streamState := "down"
	if hr, ok := s.r.(haltedReader); ok {
		if up, err := hr.wsStreamsUp(); err == nil && up {
			streamState = "up"
		}
	}
	halted := false
	if hr, ok := s.r.(haltedReader); ok {
		if h, err := hr.circuitHalted(); err == nil {
			halted = h
		}
	}
	// WHY: cheap single-column query for the timestamp + a real PingContext
	// gated through the statusReader optional capability so test stubs can
	// inject failures. On error, return 500 with a JSON error body —
	// silently serving stale data masks operational issues from the
	// operator who curls /status.
	var lastTs int64
	pgConnected := true
	if sr, ok := s.r.(statusReader); ok {
		ts, err := sr.lastSnapshotTs()
		if err != nil {
			writeStatusError(w, http.StatusInternalServerError,
				"lastSnapshotTs: "+err.Error())
			return
		}
		lastTs = ts
		// pg_connected: 2 s deadline so a hung Postgres can't stall /status.
		ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		defer cancel()
		if err := sr.dbPing(ctx); err != nil {
			writeStatusError(w, http.StatusInternalServerError,
				"pg_ping: "+err.Error())
			return
		}
	} else if snap, err := s.r.latestSnapshot(); err == nil {
		// Fallback path for stub readers in tests that don't implement statusReader.
		lastTs = snap.TsNs
	}
	// Phase 8: ws_connected proxy = any position_snapshots row in last 5s.
	wsConnected := false
	if lastTs > 0 {
		ageNs := time.Now().UnixNano() - lastTs
		if ageNs >= 0 && ageNs < int64(5)*int64(time.Second) {
			wsConnected = true
		}
	}
	var lastAgeMs int64
	if lastTs > 0 {
		lastAgeMs = (time.Now().UnixNano() - lastTs) / int64(time.Millisecond)
	}
	out := map[string]interface{}{
		"exchange": s.exchangeName,
		"ws_streams": map[string]string{
			"tickers": streamState,
			"books5":  streamState,
			"trades":  streamState,
		},
		"uptime_seconds":       int64(time.Since(s.startTime).Seconds()),
		"last_event_ts_ns":     lastTs,
		"halted":               halted,
		"ws_connected":         wsConnected,
		"pg_connected":         pgConnected,
		"last_snapshot_age_ms": lastAgeMs,
	}
	writeJSON(w, out)
}

func (s *server) handleTrades(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	limit := parseLimit(r, 100, 1000)
	trades, err := s.r.recentTrades(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, trades)
}

func (s *server) handlePnlDaily(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	d, err := s.r.dailyPnl()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, d)
}

func (s *server) handleSpreads(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	limit := parseLimit(r, 1000, 10000)
	pts, err := s.r.spreadHistory(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, pts)
}

func (s *server) handleInventory(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	limit := parseLimit(r, 1000, 10000)
	pts, err := s.r.inventoryHistory(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, pts)
}

func (s *server) handleAlerts(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, s.alerts.list())
	case http.MethodPost:
		var rule AlertRule
		if err := json.NewDecoder(r.Body).Decode(&rule); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		if rule.Type == "" || rule.Channel == "" {
			http.Error(w, "type and channel required", http.StatusBadRequest)
			return
		}
		saved := s.alerts.upsert(rule)
		writeJSON(w, saved)
	case http.MethodDelete:
		id := r.URL.Query().Get("id")
		if id == "" {
			http.Error(w, "id required", http.StatusBadRequest)
			return
		}
		if !s.alerts.delete(id) {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

// handleLogs tails the trading binary's log file. Reads the entire file
// (the file is rotated by spdlog and stays bounded so this is cheap), then
// returns the last `lines` rows.
func (s *server) handleLogs(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	lines := parseLimitStr(r.URL.Query().Get("lines"), 200, 1000)
	b, err := os.ReadFile(s.logPath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			writeJSON(w, LogsResponse{Lines: []string{}, TotalLines: 0})
			return
		}
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	all := strings.Split(strings.TrimRight(string(b), "\n"), "\n")
	if len(all) == 1 && all[0] == "" {
		writeJSON(w, LogsResponse{Lines: []string{}, TotalLines: 0})
		return
	}
	total := len(all)
	if lines < total {
		all = all[total-lines:]
	}
	writeJSON(w, LogsResponse{Lines: all, TotalLines: total})
}

// parseLimit overload-by-string: original parseLimit takes the request and
// reads "limit". This variant takes a raw string so /api/logs can read
// "lines" without a second helper.
func parseLimitStr(q string, def, cap int) int {
	if q == "" {
		return def
	}
	n, err := strconv.Atoi(q)
	if err != nil || n <= 0 {
		return def
	}
	if n > cap {
		return cap
	}
	return n
}

func (s *server) handleConfig(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	switch r.Method {
	case http.MethodGet:
		b, err := os.ReadFile(s.configPath)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = w.Write(b)
	case http.MethodPost:
		body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if len(strings.TrimSpace(string(body))) == 0 {
			http.Error(w, "empty body", http.StatusBadRequest)
			return
		}
		// Backup before overwriting. If backup fails the write still proceeds
		// but we log the issue — operators can recover from git history.
		if cur, err := os.ReadFile(s.configPath); err == nil {
			if werr := os.WriteFile(s.configPath+".bak", cur, 0644); werr != nil {
				log.Printf("config_backup_failed: %v", werr)
			}
		}
		if err := os.WriteFile(s.configPath, body, 0644); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, map[string]string{"status": "saved"})
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *server) handleBacktest(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	rows, err := readBacktestCSV(s.backtestCsv)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			writeJSON(w, []BacktestRow{})
			return
		}
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, rows)
}

func (s *server) handleBacktestRun(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := s.backtestRunner(); err != nil {
		writeStatusError(w, http.StatusInternalServerError, "spawn: "+err.Error())
		return
	}
	writeJSON(w, map[string]string{"status": "started"})
}

func (s *server) handleOrders(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	snap, err := s.r.latestSnapshot()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	limit := parseLimit(r, 50, 500)
	evs, err := s.r.orderEvents(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, OrdersPayload{OpenCount: snap.OpenOrders, Events: evs})
}

// getFundingRate returns the cached value if fresh, otherwise refetches.
// 30s TTL — funding rate updates every 8h on OKX so 30s is comfortably
// inside any meaningful change window.
func (s *server) getFundingRate(ctx context.Context) (FundingRate, error) {
	s.fundingMu.Lock()
	defer s.fundingMu.Unlock()
	if !s.fundingAt.IsZero() && time.Since(s.fundingAt) < s.fundingTTL {
		return s.fundingVal, s.fundingErr
	}
	val, err := s.fundingFetcher(ctx)
	s.fundingVal = val
	s.fundingErr = err
	s.fundingAt = time.Now()
	return val, err
}

func (s *server) handleFundingRate(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 6*time.Second)
	defer cancel()
	val, err := s.getFundingRate(ctx)
	if err != nil {
		writeStatusError(w, http.StatusBadGateway, "okx: "+err.Error())
		return
	}
	writeJSON(w, val)
}

func (s *server) handleCalibration(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	rows, err := readCalibrationCSV(s.calibrationCsv)
	if err != nil {
		// Empty CSV / missing file isn't a 500 — it's "no calibration has been
		// run yet" and the UI should render an empty table.
		if errors.Is(err, os.ErrNotExist) {
			writeJSON(w, []CalibrationRow{})
			return
		}
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, rows)
}

func (s *server) handleCalibrationRun(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := s.calibrationRunner(); err != nil {
		writeStatusError(w, http.StatusInternalServerError, "spawn: "+err.Error())
		return
	}
	writeJSON(w, map[string]string{"status": "started"})
}

// readCalibrationCSV parses the top-N CSV produced by the C++ calibration
// sweep. The C++ writer emits a header row followed by data rows with
// columns gamma, k, T (or horizon), sharpe, pnl, max_dd, fills (case-insensitive,
// extra columns ignored). Order is matched by header lookup.
func readCalibrationCSV(path string) ([]CalibrationRow, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	rd := csv.NewReader(f)
	rd.FieldsPerRecord = -1
	records, err := rd.ReadAll()
	if err != nil {
		return nil, err
	}
	if len(records) == 0 {
		return []CalibrationRow{}, nil
	}
	idx := map[string]int{}
	for i, h := range records[0] {
		idx[strings.ToLower(strings.TrimSpace(h))] = i
	}
	get := func(row []string, names ...string) string {
		for _, n := range names {
			if i, ok := idx[n]; ok && i < len(row) {
				return row[i]
			}
		}
		return ""
	}
	out := make([]CalibrationRow, 0, len(records)-1)
	for _, row := range records[1:] {
		if len(row) == 0 {
			continue
		}
		gamma, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "gamma", "γ")), 64)
		k, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "k")), 64)
		t, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "t", "horizon")), 64)
		sharpe, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "sharpe")), 64)
		pnl, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "pnl", "p&l", "p_n_l")), 64)
		maxDd, _ := strconv.ParseFloat(strings.TrimSpace(get(row, "max_dd", "maxdd", "max_drawdown")), 64)
		fills, _ := strconv.Atoi(strings.TrimSpace(get(row, "fills", "fill_count")))
		out = append(out, CalibrationRow{
			Gamma: gamma, K: k, T: t, Sharpe: sharpe, Pnl: pnl, MaxDD: maxDd, Fills: fills,
		})
	}
	return out, nil
}

func (s *server) handleEvents(w http.ResponseWriter, r *http.Request) {
	s.cors(w, r)
	if r.Method == http.MethodOptions {
		return
	}
	limit := parseLimit(r, 100, 1000)
	evs, err := s.r.recentEvents(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, evs)
}

// WHY: Minimal RFC 6455 server. We only send text frames (no fragmentation,
// no client-to-server traffic processing beyond close).
const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

func wsAccept(key string) string {
	h := sha1.New()
	h.Write([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

func (s *server) handleWS(w http.ResponseWriter, r *http.Request) {
	if strings.ToLower(r.Header.Get("Upgrade")) != "websocket" {
		http.Error(w, "expected websocket upgrade", http.StatusBadRequest)
		return
	}
	// RFC 6455 §4.2.1: server MUST verify Sec-WebSocket-Version is 13. If not,
	// respond with 426 and Sec-WebSocket-Version listing the version we speak.
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "unsupported websocket version", http.StatusUpgradeRequired)
		return
	}
	// WHY: CORS does not gate WS — without an origin check here, any internet
	// page can open a socket to the dashboard.
	if !s.originAllowed(r.Header.Get("Origin")) {
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing key", http.StatusBadRequest)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "no hijacker", http.StatusInternalServerError)
		return
	}
	conn, bufrw, err := hj.Hijack()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer conn.Close()

	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + wsAccept(key) + "\r\n\r\n"
	if _, err := bufrw.WriteString(resp); err != nil {
		return
	}
	if err := bufrw.Flush(); err != nil {
		return
	}

	// WHY: reader goroutine forwards ping payloads to pongCh so the MAIN
	// goroutine remains the sole writer on conn (net.Conn allows concurrent
	// Read+Write but not concurrent Writes; interleaved frame bytes corrupt
	// the stream).
	pongCh := make(chan []byte, 8)
	done := make(chan struct{})
	go func() {
		defer close(done)
		readClient(conn, bufrw.Reader, pongCh)
	}()

	ticker := time.NewTicker(s.wsInterval)
	defer ticker.Stop()
	for {
		select {
		case <-done:
			return
		case payload := <-pongCh:
			if err := writePongFrame(conn, payload); err != nil {
				return
			}
		case <-ticker.C:
			snap, err := s.r.latestSnapshot()
			if err != nil {
				continue
			}
			s.enrichSnapshot(&snap)
			if s.alerts != nil && s.alertFire != nil {
				s.alerts.evaluate(snap, s.alertFire)
			}
			payload, err := json.Marshal(snap)
			if err != nil {
				continue
			}
			if err := writeTextFrame(conn, payload); err != nil {
				return
			}
		}
	}
}

// writeFrame writes a single unfragmented server-to-client frame. opcode is
// the low 4 bits of the first byte (0x1 text, 0xA pong); FIN bit always set.
func writeFrame(conn net.Conn, opcode byte, payload []byte) error {
	_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	first := byte(0x80) | (opcode & 0x0f)
	n := len(payload)
	var hdr []byte
	switch {
	case n <= 125:
		hdr = []byte{first, byte(n)}
	case n <= 65535:
		hdr = []byte{first, 126, 0, 0}
		binary.BigEndian.PutUint16(hdr[2:], uint16(n))
	default:
		hdr = make([]byte, 10)
		hdr[0] = first
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:], uint64(n))
	}
	if _, err := conn.Write(hdr); err != nil {
		return err
	}
	if n == 0 {
		return nil
	}
	_, err := conn.Write(payload)
	return err
}

func writeTextFrame(conn net.Conn, payload []byte) error {
	return writeFrame(conn, 0x1, payload)
}

func writePongFrame(conn net.Conn, payload []byte) error {
	return writeFrame(conn, 0xA, payload)
}

func readClient(conn net.Conn, rd *bufio.Reader, pongCh chan<- []byte) {
	// Read frames. On close opcode (0x8) or EOF return so the writer can exit.
	// On ping (0x9) dispatch the (unmasked) payload to pongCh; the main
	// goroutine sends the pong reply so writes stay serialized on one side.
	hdr := make([]byte, 2)
	for {
		if _, err := io.ReadFull(rd, hdr); err != nil {
			return
		}
		opcode := hdr[0] & 0x0f
		masked := (hdr[1] & 0x80) != 0
		plen := int(hdr[1] & 0x7f)
		switch plen {
		case 126:
			ext := make([]byte, 2)
			if _, err := io.ReadFull(rd, ext); err != nil {
				return
			}
			plen = int(binary.BigEndian.Uint16(ext))
		case 127:
			ext := make([]byte, 8)
			if _, err := io.ReadFull(rd, ext); err != nil {
				return
			}
			plen = int(binary.BigEndian.Uint64(ext))
		}
		var payload []byte
		if masked {
			mask := make([]byte, 4)
			if _, err := io.ReadFull(rd, mask); err != nil {
				return
			}
			payload = make([]byte, plen)
			if _, err := io.ReadFull(rd, payload); err != nil {
				return
			}
			for i := range payload {
				payload[i] ^= mask[i%4]
			}
		} else if plen > 0 {
			// Client-to-server frames MUST be masked per RFC; an unmasked
			// frame is a protocol violation. Discard payload and continue —
			// strict handling would send a close 1002.
			if _, err := io.CopyN(io.Discard, rd, int64(plen)); err != nil {
				return
			}
		}
		switch opcode {
		case 0x8:
			return
		case 0x9:
			// Ping control frames must carry ≤ 125 bytes. Echo payload as pong.
			select {
			case pongCh <- payload:
			default:
				// pongCh full — drop; not worth blocking the reader.
			}
		}
	}
}

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/snapshot", s.handleSnapshot)
	mux.HandleFunc("/api/trades", s.handleTrades)
	mux.HandleFunc("/api/pnl/daily", s.handlePnlDaily)
	mux.HandleFunc("/api/events", s.handleEvents)
	mux.HandleFunc("/api/spreads", s.handleSpreads)
	mux.HandleFunc("/api/inventory", s.handleInventory)
	mux.HandleFunc("/api/orders", s.handleOrders)
	mux.HandleFunc("/api/funding-rate", s.handleFundingRate)
	mux.HandleFunc("/api/calibration", s.handleCalibration)
	mux.HandleFunc("/api/calibration/run", s.handleCalibrationRun)
	mux.HandleFunc("/api/backtest", s.handleBacktest)
	mux.HandleFunc("/api/backtest/run", s.handleBacktestRun)
	mux.HandleFunc("/api/alerts", s.handleAlerts)
	mux.HandleFunc("/api/logs", s.handleLogs)
	mux.HandleFunc("/api/config", s.handleConfig)
	mux.HandleFunc("/api/v5/status", s.handleStatus)
	mux.HandleFunc("/ws", s.handleWS)
	// TODO(phase 7): auth.
	return mux
}
