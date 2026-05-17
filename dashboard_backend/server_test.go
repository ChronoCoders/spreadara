package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

// statusStub adds statusReader to the base stubReader so /api/v5/status
// failure paths are reachable from tests without a live PG connection.
type statusStub struct {
	stubReader
	ts      int64
	tsErr   error
	pingErr error
}

func (s *statusStub) lastSnapshotTs() (int64, error)         { return s.ts, s.tsErr }
func (s *statusStub) dbPing(_ context.Context) error          { return s.pingErr }

type stubReader struct {
	snap Snapshot
}

func (s *stubReader) latestSnapshot() (Snapshot, error) { return s.snap, nil }
func (s *stubReader) recentTrades(limit int) ([]Trade, error) {
	return []Trade{{OrderID: "x", Side: 1, Price: 1.0, Qty: 0.1}}, nil
}
func (s *stubReader) dailyPnl() ([]DailyPnl, error) { return []DailyPnl{}, nil }
func (s *stubReader) recentEvents(limit int) ([]SystemEvent, error) {
	return []SystemEvent{}, nil
}
func (s *stubReader) spreadHistory(limit int) ([]SpreadPoint, error) {
	return []SpreadPoint{{TsNs: 1, SpreadBps: 1.5}, {TsNs: 2, SpreadBps: 2.0}}, nil
}
func (s *stubReader) inventoryHistory(limit int) ([]InventoryPoint, error) {
	return []InventoryPoint{{TsNs: 1, Inventory: 0.05, MidPrice: 78000.0}}, nil
}

func TestSnapshotHandler(t *testing.T) {
	stub := &stubReader{snap: Snapshot{Inventory: 1.5, Mid: 30000.0}}
	srv := newServer(stub, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/snapshot")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status = %d", resp.StatusCode)
	}
	var got Snapshot
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatal(err)
	}
	if got.Inventory != 1.5 || got.Mid != 30000.0 {
		t.Fatalf("unexpected snapshot: %+v", got)
	}
}

func TestSpreadsHandler(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	resp, err := http.Get(ts.URL + "/api/spreads?limit=10")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status = %d", resp.StatusCode)
	}
	var got []SpreadPoint
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].SpreadBps != 1.5 {
		t.Fatalf("unexpected spreads: %+v", got)
	}
}

func TestInventoryHandler(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	resp, err := http.Get(ts.URL + "/api/inventory?limit=10")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status = %d", resp.StatusCode)
	}
	var got []InventoryPoint
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].MidPrice != 78000.0 {
		t.Fatalf("unexpected inventory: %+v", got)
	}
}

func TestUnknownRoute404(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	resp, err := http.Get(ts.URL + "/api/does-not-exist")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 404 {
		t.Fatalf("expected 404, got %d", resp.StatusCode)
	}
}

func TestTradesHandler(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	resp, err := http.Get(ts.URL + "/api/trades?limit=5")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status = %d", resp.StatusCode)
	}
	var got []Trade
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 {
		t.Fatalf("expected 1 trade, got %d", len(got))
	}
}

// readFrame reads a single unmasked text frame from the server.
func readFrame(r io.Reader) ([]byte, error) {
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return nil, err
	}
	plen := int(hdr[1] & 0x7f)
	switch plen {
	case 126:
		ext := make([]byte, 2)
		if _, err := io.ReadFull(r, ext); err != nil {
			return nil, err
		}
		plen = int(binary.BigEndian.Uint16(ext))
	case 127:
		ext := make([]byte, 8)
		if _, err := io.ReadFull(r, ext); err != nil {
			return nil, err
		}
		plen = int(binary.BigEndian.Uint64(ext))
	}
	payload := make([]byte, plen)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func TestWSVersionReject(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	req, _ := http.NewRequest("GET", ts.URL+"/ws", nil)
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")
	req.Header.Set("Sec-WebSocket-Version", "8") // wrong
	req.Header.Set("Origin", ts.URL)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusUpgradeRequired {
		t.Fatalf("expected 426, got %d", resp.StatusCode)
	}
	if resp.Header.Get("Sec-WebSocket-Version") != "13" {
		t.Fatalf("expected Sec-WebSocket-Version: 13 hint, got %q",
			resp.Header.Get("Sec-WebSocket-Version"))
	}
}

func TestWSOriginReject(t *testing.T) {
	srv := newServer(&stubReader{}, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()
	req, _ := http.NewRequest("GET", ts.URL+"/ws", nil)
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")
	req.Header.Set("Sec-WebSocket-Version", "13")
	req.Header.Set("Origin", "http://evil.example.com")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("expected 403, got %d", resp.StatusCode)
	}
}

func TestWSPushInterval(t *testing.T) {
	stub := &stubReader{snap: Snapshot{Inventory: 0.5}}
	srv := newServer(stub, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()

	u, _ := url.Parse(ts.URL)
	// Hijacker is supported on httptest's net/http server.
	conn, err := dialWS(u.Host, "/ws")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()

	deadline := time.Now().Add(300 * time.Millisecond)
	_ = conn.SetReadDeadline(deadline)
	count := 0
	for {
		if _, err := readFrame(conn); err != nil {
			break
		}
		count++
	}
	// 250ms window at 50ms interval should be 4-6 frames. Allow 3-7 for CI jitter.
	if count < 3 || count > 7 {
		t.Fatalf("expected 3-7 frames, got %d", count)
	}
}

// dialWS does a minimal client handshake.
func dialWS(host, path string) (interface {
	io.ReadWriteCloser
	SetReadDeadline(time.Time) error
}, error) {
	conn, err := dialTCP(host)
	if err != nil {
		return nil, err
	}
	req := "GET " + path + " HTTP/1.1\r\n" +
		"Host: " + host + "\r\n" +
		"Origin: http://" + host + "\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
		"Sec-WebSocket-Version: 13\r\n\r\n"
	if _, err := conn.Write([]byte(req)); err != nil {
		conn.Close()
		return nil, err
	}
	// Read response headers until \r\n\r\n
	buf := make([]byte, 1)
	hdr := make([]byte, 0, 256)
	for {
		if _, err := conn.Read(buf); err != nil {
			conn.Close()
			return nil, err
		}
		hdr = append(hdr, buf[0])
		if strings.HasSuffix(string(hdr), "\r\n\r\n") {
			break
		}
	}
	return conn, nil
}

func TestStatusHappyPath(t *testing.T) {
	stub := &statusStub{
		stubReader: stubReader{snap: Snapshot{TsNs: time.Now().UnixNano(), Mid: 30000}},
		ts:         time.Now().UnixNano(),
	}
	srv := newServer(stub, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/v5/status")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
	var body map[string]interface{}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	for _, k := range []string{"exchange", "ws_streams", "uptime_seconds",
		"last_event_ts_ns", "halted", "ws_connected", "pg_connected", "last_snapshot_age_ms"} {
		if _, ok := body[k]; !ok {
			t.Errorf("missing key %q in status JSON: %v", k, body)
		}
	}
	if body["pg_connected"] != true {
		t.Errorf("pg_connected = %v, want true", body["pg_connected"])
	}
}

func TestStatusTsQueryFailure(t *testing.T) {
	stub := &statusStub{tsErr: errors.New("connection refused")}
	srv := newServer(stub, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/v5/status")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 500 {
		t.Fatalf("status = %d, want 500", resp.StatusCode)
	}
	var body map[string]string
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(body["error"], "lastSnapshotTs") {
		t.Errorf("error body = %v, want substring 'lastSnapshotTs'", body)
	}
}

func TestStatusPingFailure(t *testing.T) {
	stub := &statusStub{
		ts:      time.Now().UnixNano(),
		pingErr: errors.New("server closed the connection"),
	}
	srv := newServer(stub, 50*time.Millisecond, "")
	ts := httptest.NewServer(srv.routes())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/v5/status")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 500 {
		t.Fatalf("status = %d, want 500", resp.StatusCode)
	}
	var body map[string]string
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(body["error"], "pg_ping") {
		t.Errorf("error body = %v, want substring 'pg_ping'", body)
	}
}
