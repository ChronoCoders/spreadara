// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"time"
)

type User struct {
	ID          int64
	Email       string
	PasswordHash sql.NullString
	Role        string
	InviteToken  sql.NullString
	InvitedAt   sql.NullTime
	ActivatedAt sql.NullTime
	CreatedAt   time.Time
}

type userStore interface {
	getUserByEmail(email string) (*User, error)
	getUserByInviteToken(token string) (*User, error)
	createUser(email, role string) (*User, error)
	activateUser(id int64, passwordHash string) error
	updateRefreshToken(id int64, token string) error
	validateRefreshToken(id int64, token string) (bool, error)
	listUsers() ([]User, error)
	deleteUser(id int64) error
	hasUsers() (bool, error)
	getUserByID(id int64) (*User, error)
}

type sqlUserStore struct {
	db *sql.DB
}

func createUsersTable(db *sql.DB) error {
	_, err := db.Exec(`CREATE TABLE IF NOT EXISTS users (
		id              BIGSERIAL PRIMARY KEY,
		email           VARCHAR(255) UNIQUE NOT NULL,
		password_hash   VARCHAR(255),
		role            VARCHAR(20) NOT NULL DEFAULT 'viewer',
		invite_token    VARCHAR(64),
		invited_at      TIMESTAMPTZ,
		activated_at    TIMESTAMPTZ,
		refresh_token_hash VARCHAR(64),
		created_at      TIMESTAMPTZ DEFAULT NOW()
	)`)
	return err
}

func hashRefreshToken(token string) string {
	h := sha256.Sum256([]byte(token))
	return hex.EncodeToString(h[:])
}

func (s *sqlUserStore) getUserByEmail(email string) (*User, error) {
	u := &User{}
	err := s.db.QueryRow(
		`SELECT id, email, password_hash, role, invite_token, invited_at, activated_at, created_at
		 FROM users WHERE email = $1`, email,
	).Scan(&u.ID, &u.Email, &u.PasswordHash, &u.Role, &u.InviteToken, &u.InvitedAt, &u.ActivatedAt, &u.CreatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return u, err
}

func (s *sqlUserStore) getUserByID(id int64) (*User, error) {
	u := &User{}
	err := s.db.QueryRow(
		`SELECT id, email, password_hash, role, invite_token, invited_at, activated_at, created_at
		 FROM users WHERE id = $1`, id,
	).Scan(&u.ID, &u.Email, &u.PasswordHash, &u.Role, &u.InviteToken, &u.InvitedAt, &u.ActivatedAt, &u.CreatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return u, err
}

func (s *sqlUserStore) getUserByInviteToken(token string) (*User, error) {
	u := &User{}
	err := s.db.QueryRow(
		`SELECT id, email, password_hash, role, invite_token, invited_at, activated_at, created_at
		 FROM users WHERE invite_token = $1`, token,
	).Scan(&u.ID, &u.Email, &u.PasswordHash, &u.Role, &u.InviteToken, &u.InvitedAt, &u.ActivatedAt, &u.CreatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return u, err
}

func (s *sqlUserStore) createUser(email, role string) (*User, error) {
	token, err := generateInviteToken()
	if err != nil {
		return nil, err
	}
	now := time.Now()
	u := &User{}
	err = s.db.QueryRow(
		`INSERT INTO users (email, role, invite_token, invited_at)
		 VALUES ($1, $2, $3, $4)
		 RETURNING id, email, password_hash, role, invite_token, invited_at, activated_at, created_at`,
		email, role, token, now,
	).Scan(&u.ID, &u.Email, &u.PasswordHash, &u.Role, &u.InviteToken, &u.InvitedAt, &u.ActivatedAt, &u.CreatedAt)
	return u, err
}

func (s *sqlUserStore) activateUser(id int64, passwordHash string) error {
	_, err := s.db.Exec(
		`UPDATE users SET password_hash = $1, activated_at = NOW(), invite_token = NULL WHERE id = $2`,
		passwordHash, id,
	)
	return err
}

func (s *sqlUserStore) updateRefreshToken(id int64, token string) error {
	_, err := s.db.Exec(
		`UPDATE users SET refresh_token_hash = $1 WHERE id = $2`,
		hashRefreshToken(token), id,
	)
	return err
}

func (s *sqlUserStore) validateRefreshToken(id int64, token string) (bool, error) {
	var stored sql.NullString
	err := s.db.QueryRow(`SELECT refresh_token_hash FROM users WHERE id = $1`, id).Scan(&stored)
	if err != nil {
		return false, err
	}
	if !stored.Valid {
		return false, nil
	}
	return stored.String == hashRefreshToken(token), nil
}

func (s *sqlUserStore) listUsers() ([]User, error) {
	rows, err := s.db.Query(
		`SELECT id, email, role, activated_at, created_at FROM users ORDER BY created_at`,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []User
	for rows.Next() {
		var u User
		if err := rows.Scan(&u.ID, &u.Email, &u.Role, &u.ActivatedAt, &u.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, u)
	}
	return out, rows.Err()
}

func (s *sqlUserStore) deleteUser(id int64) error {
	_, err := s.db.Exec(`DELETE FROM users WHERE id = $1`, id)
	return err
}

func (s *sqlUserStore) hasUsers() (bool, error) {
	var n int
	err := s.db.QueryRow(`SELECT COUNT(*) FROM users`).Scan(&n)
	return n > 0, err
}
