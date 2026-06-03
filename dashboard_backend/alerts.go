// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

package main

// alertStore manages persistent alert rules and evaluates them against
// snapshots. Storage is a JSON file (path configurable). Each rule has a
// 60s cooldown per ID to avoid spamming the channel while a condition
// remains breached.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

const alertCooldown = 60 * time.Second

type alertStore struct {
	path     string
	mu       sync.Mutex
	rules    []AlertRule
	lastFire map[string]time.Time
}

func newAlertStore(path string) *alertStore {
	s := &alertStore{path: path, lastFire: map[string]time.Time{}}
	_ = s.load()
	return s
}

func (s *alertStore) load() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	b, err := os.ReadFile(s.path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			s.rules = []AlertRule{}
			return nil
		}
		return err
	}
	if len(bytes.TrimSpace(b)) == 0 {
		s.rules = []AlertRule{}
		return nil
	}
	var rules []AlertRule
	if err := json.Unmarshal(b, &rules); err != nil {
		return err
	}
	s.rules = rules
	return nil
}

func (s *alertStore) save() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.rules == nil {
		s.rules = []AlertRule{}
	}
	b, err := json.MarshalIndent(s.rules, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(s.path, b, 0644)
}

func (s *alertStore) list() []AlertRule {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]AlertRule, len(s.rules))
	copy(out, s.rules)
	return out
}

// upsert replaces a rule by ID or appends if not found. Empty ID generates one.
func (s *alertStore) upsert(rule AlertRule) AlertRule {
	s.mu.Lock()
	if rule.ID == "" {
		rule.ID = generateID()
	}
	found := false
	for i, r := range s.rules {
		if r.ID == rule.ID {
			s.rules[i] = rule
			found = true
			break
		}
	}
	if !found {
		s.rules = append(s.rules, rule)
	}
	s.mu.Unlock()
	_ = s.save()
	return rule
}

func (s *alertStore) delete(id string) bool {
	s.mu.Lock()
	out := s.rules[:0]
	deleted := false
	for _, r := range s.rules {
		if r.ID == id {
			deleted = true
			continue
		}
		out = append(out, r)
	}
	s.rules = out
	s.mu.Unlock()
	if deleted {
		_ = s.save()
	}
	return deleted
}

func generateID() string {
	return time.Now().UTC().Format("20060102T150405.000000000")
}

// evaluate checks each enabled rule against the snapshot and fires any that
// breach their threshold (and aren't on cooldown). Best-effort, non-blocking:
// each fire runs in its own goroutine and never returns an error to the
// caller. WHY: this is invoked from the snapshot push path, which must not
// stall behind a slow webhook.
func (s *alertStore) evaluate(snap Snapshot, fire alertFirer) {
	s.mu.Lock()
	rules := make([]AlertRule, len(s.rules))
	copy(rules, s.rules)
	now := time.Now()
	for _, r := range rules {
		if !r.Enabled {
			continue
		}
		if !ruleBreached(r, snap) {
			continue
		}
		if last, ok := s.lastFire[r.ID]; ok && now.Sub(last) < alertCooldown {
			continue
		}
		s.lastFire[r.ID] = now
		// Copy r into the goroutine closure.
		rule := r
		go fire(rule, snap)
	}
	s.mu.Unlock()
}

func ruleBreached(r AlertRule, s Snapshot) bool {
	switch strings.ToLower(r.Type) {
	case "drawdown":
		return s.CurrentDrawdownPct >= r.Threshold
	case "inventory":
		if r.Threshold <= 0 {
			return false
		}
		return absF(s.Inventory) >= r.Threshold
	case "pnl":
		// Threshold negative — fire when realized P&L falls below it.
		// Threshold positive — fire when realized P&L rises above it.
		if r.Threshold < 0 {
			return s.Realized <= r.Threshold
		}
		return s.Realized >= r.Threshold
	case "circuit_breaker":
		return s.CircuitBreakerHalted
	}
	return false
}

func absF(v float64) float64 {
	if v < 0 {
		return -v
	}
	return v
}

// sanitizeURLError renders a request error without leaking the webhook URL,
// which may embed a secret token in its path or query. *url.Error carries the
// full URL in both its .URL field and its formatted message, so we report the
// operation + underlying cause instead, and defensively strip any literal
// occurrence of the URL from the result.
func sanitizeURLError(err error, webhookURL string) string {
	msg := err.Error()
	var uerr *url.Error
	if errors.As(err, &uerr) {
		cause := "?"
		if uerr.Err != nil {
			cause = uerr.Err.Error()
		}
		msg = uerr.Op + ": " + cause
	}
	if webhookURL != "" {
		msg = strings.ReplaceAll(msg, webhookURL, "[redacted]")
	}
	return msg
}

type alertFirer func(rule AlertRule, snap Snapshot)

// defaultAlertFirer logs every fire and posts to webhook if configured.
// Webhook POST has a 3s timeout; errors are logged but not surfaced.
func defaultAlertFirer(rule AlertRule, snap Snapshot) {
	log.Printf("alert_fire id=%s type=%s threshold=%g channel=%s",
		rule.ID, rule.Type, rule.Threshold, rule.Channel)
	if rule.Channel != "webhook" || rule.WebhookURL == "" {
		return
	}
	body := map[string]interface{}{
		"rule_id":   rule.ID,
		"rule_name": rule.Name,
		"type":      rule.Type,
		"threshold": rule.Threshold,
		"fired_at":  time.Now().UTC().Format(time.RFC3339Nano),
		"snapshot":  snap,
	}
	b, err := json.Marshal(body)
	if err != nil {
		log.Printf("alert_webhook_marshal_error: %v", err)
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "POST", rule.WebhookURL, bytes.NewReader(b))
	if err != nil {
		log.Printf("alert_webhook_req_error: %v", err)
		return
	}
	req.Header.Set("Content-Type", "application/json")
	client := &http.Client{Timeout: 3 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		log.Printf("alert_webhook_post_error rule=%s: %s", rule.ID, sanitizeURLError(err, rule.WebhookURL))
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		// Drain a bounded amount of the response body so the conn can be
		// reused, but don't log it (could leak third-party content).
		_, _ = io.CopyN(io.Discard, resp.Body, 1024)
		log.Printf("alert_webhook_non_2xx rule=%s status=%d", rule.ID, resp.StatusCode)
	}
}
