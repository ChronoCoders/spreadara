// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

package main

// WHY: Standalone Go sidecar reads only from Postgres. CGO_ENABLED=0,
// stdlib net/http only (plus pure-Go lib/pq). Config via env vars per
// spec rule (no hardcoded URLs/ports). DSN comes solely from
// SPREADARA_PG_DSN and is never logged.

import (
	"database/sql"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"
	"time"

	_ "github.com/lib/pq"
)

func mustEnv(name string) string {
	v := os.Getenv(name)
	if v == "" {
		log.Printf("missing required env var: %s", name)
		os.Exit(2)
	}
	return v
}

func envInt(name string, def int) int {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		log.Printf("bad %s=%q, using default %d", name, v, def)
		return def
	}
	return n
}

func envStr(name, def string) string {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	return v
}

func main() {
	dsn := mustEnv("SPREADARA_PG_DSN")
	jwtSecret := mustEnv("SPREADARA_JWT_SECRET")
	port := envInt("SPREADARA_DASHBOARD_PORT", 8080)
	intervalMs := envInt("SPREADARA_DASHBOARD_INTERVAL_MS", 500)
	corsOrigin := envStr("SPREADARA_DASHBOARD_CORS_ORIGIN", "")
	// WHY: bind to loopback by default so the dashboard backend is never
	// directly reachable off-box; a fronting reverse proxy forwards to it.
	// Overridable via env for setups that intentionally bind elsewhere.
	host := envStr("SPREADARA_DASHBOARD_HOST", "127.0.0.1")

	db, err := sql.Open("postgres", dsn)
	if err != nil {
		log.Fatalf("sql.Open failed: %v", err)
	}
	defer db.Close()
	db.SetMaxOpenConns(8)
	db.SetMaxIdleConns(2)
	if err := db.Ping(); err != nil {
		log.Printf("warn: db ping failed at startup: %v", err)
	}

	if err := createUsersTable(db); err != nil {
		log.Printf("warn: createUsersTable: %v", err)
	} else {
		seedFirstAdmin(db)
	}

	srv := newServer(&sqlReader{db: db}, time.Duration(intervalMs)*time.Millisecond, corsOrigin).
		withDB(db).
		withJWT([]byte(jwtSecret))
	mux := srv.routes()

	addr := host + ":" + strconv.Itoa(port)
	log.Printf("dashboard_backend listening on %s interval_ms=%d", addr, intervalMs)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatalf("ListenAndServe: %v", err)
	}
}

func seedFirstAdmin(db *sql.DB) {
	adminEmail := os.Getenv("SPREADARA_ADMIN_EMAIL")
	if adminEmail == "" {
		return
	}
	store := &sqlUserStore{db: db}
	has, err := store.hasUsers()
	if err != nil || has {
		return
	}
	u, err := store.createUser(adminEmail, "admin")
	if err != nil {
		log.Printf("warn: seed_first_admin create failed: %v", err)
		return
	}
	appURL := envStr("SPREADARA_APP_URL", "http://localhost:5173")
	inviteURL := fmt.Sprintf("%s/accept-invite?token=%s", appURL, u.InviteToken.String)
	log.Printf("first_admin_invite email=%s invite_url=%s", u.Email, inviteURL)
	if err := sendInviteEmail(u.Email, u.InviteToken.String); err != nil {
		log.Printf("warn: send_invite_email: %v", err)
	}
}
