package main

// WHY: dbReader interface lets tests inject a stub without a live Postgres.
// WS implementation: manual RFC 6455 handshake + frame writer using
// net/http's Hijacker (chosen over x/net/websocket so go.sum stays minimal
// with only lib/pq — gorilla/websocket disallowed per spec).

import (
	"bufio"
	"crypto/sha1"
	"database/sql"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
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
}

type Trade struct {
	TsNs     int64   `json:"ts_ns"`
	OrderID  string  `json:"order_id"`
	Side     int     `json:"side"`
	Price    float64 `json:"price"`
	Qty      float64 `json:"qty"`
	Fee      float64 `json:"fee"`
	FeeAsset string  `json:"fee_asset"`
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

type dbReader interface {
	latestSnapshot() (Snapshot, error)
	recentTrades(limit int) ([]Trade, error)
	dailyPnl() ([]DailyPnl, error)
	recentEvents(limit int) ([]SystemEvent, error)
}

type sqlReader struct{ db *sql.DB }

func (r *sqlReader) latestSnapshot() (Snapshot, error) {
	var s Snapshot
	err := r.db.QueryRow(
		`SELECT ts_ns, inventory, avg_entry, realized_pnl, unrealized_pnl, total_fees, mid_price
		 FROM position_snapshots ORDER BY ts_ns DESC LIMIT 1`,
	).Scan(&s.TsNs, &s.Inventory, &s.AvgEntry, &s.Realized, &s.Unrealized, &s.Fees, &s.Mid)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return s, err
	}
	_ = r.db.QueryRow(`SELECT COALESCE(SUM(total),0) FROM daily_pnl`).Scan(&s.CumTotal)
	return s, nil
}

func (r *sqlReader) recentTrades(limit int) ([]Trade, error) {
	rows, err := r.db.Query(
		`SELECT ts_ns, order_id, side, price, qty, fee, fee_asset
		 FROM trades ORDER BY ts_ns DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Trade{}
	for rows.Next() {
		var t Trade
		var side int16
		if err := rows.Scan(&t.TsNs, &t.OrderID, &side, &t.Price, &t.Qty, &t.Fee, &t.FeeAsset); err != nil {
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
}

func newServer(r dbReader, wsInterval time.Duration, corsOrigin string) *server {
	return &server{r: r, wsInterval: wsInterval, corsOrigin: corsOrigin}
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
	writeJSON(w, snap)
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
	mux.HandleFunc("/ws", s.handleWS)
	// TODO(phase 7): auth.
	return mux
}
