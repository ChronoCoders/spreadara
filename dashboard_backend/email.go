// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"time"
)

var resendHTTP = &http.Client{Timeout: 10 * time.Second}

func sendInviteEmail(to, inviteToken string) error {
	apiKey := os.Getenv("SPREADARA_RESEND_API_KEY")
	appURL := os.Getenv("SPREADARA_APP_URL")
	if apiKey == "" || appURL == "" {
		return nil
	}

	link := fmt.Sprintf("%s/accept-invite?token=%s", appURL, inviteToken)
	body := map[string]interface{}{
		"from":    "noreply@spreadara.com",
		"to":      []string{to},
		"subject": "You've been invited to Spreadara",
		"html": fmt.Sprintf(`<p>You've been invited to Spreadara.</p>
<p><a href="%s">Accept your invitation</a></p>
<p>This link expires in 30 days.</p>`, link),
	}

	b, err := json.Marshal(body)
	if err != nil {
		return err
	}
	req, err := http.NewRequest("POST", "https://api.resend.com/emails", bytes.NewReader(b))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+apiKey)
	req.Header.Set("Content-Type", "application/json")

	resp, err := resendHTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("resend returned %d", resp.StatusCode)
	}
	return nil
}
