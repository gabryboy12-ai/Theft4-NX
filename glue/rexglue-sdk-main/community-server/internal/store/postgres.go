package store

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
	"libertyrecomp/community-server/internal/domain"
)

type Postgres struct{ pool *pgxpool.Pool }

func NewPostgres(ctx context.Context, url string) (*Postgres, error) {
	pool, err := pgxpool.New(ctx, url)
	if err != nil {
		return nil, err
	}
	p := &Postgres{pool: pool}
	if err := p.Ready(ctx); err != nil {
		pool.Close()
		return nil, err
	}
	return p, nil
}
func (p *Postgres) Ready(ctx context.Context) error { return p.pool.Ping(ctx) }
func (p *Postgres) Close()                          { p.pool.Close() }

func (p *Postgres) Maintain(ctx context.Context, now time.Time) error {
	tx, err := p.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	statements := []struct {
		query string
		args  []any
	}{
		{`DELETE FROM device_challenges WHERE used OR expires_at<=$1`, []any{now}},
		{`DELETE FROM refresh_sessions WHERE revoked OR expires_at<=$1`, []any{now}},
		{`DELETE FROM idempotency_keys WHERE expires_at<=$1`, []any{now}},
		{`DELETE FROM state_documents WHERE kind='session' AND (body->>'host_lease_expires_at')::timestamptz<=$1`, []any{now}},
		{`DELETE FROM state_documents WHERE kind IN ('presence','voice-route','invite-record') AND (body->>'expires_at')::timestamptz<=$1`, []any{now}},
		{`DELETE FROM state_documents WHERE kind='ticket' AND updated_at<=$1`, []any{now.Add(-10 * time.Minute)}},
		{`DELETE FROM events WHERE delivered_at IS NOT NULL AND delivered_at<=$1`, []any{now.Add(-30 * 24 * time.Hour)}},
	}
	for _, statement := range statements {
		if _, err := tx.Exec(ctx, statement.query, statement.args...); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

func scanDocument(row pgx.Row) (domain.Document, error) {
	var d domain.Document
	err := row.Scan(&d.Kind, &d.ID, &d.OwnerID, &d.Body, &d.Version, &d.CreatedAt, &d.UpdatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return d, domain.ErrNotFound
	}
	return d, err
}
func (p *Postgres) CreateDocument(ctx context.Context, d domain.Document) (domain.Document, error) {
	tx, beginErr := p.pool.Begin(ctx)
	if beginErr != nil {
		return d, beginErr
	}
	defer tx.Rollback(ctx)
	row := tx.QueryRow(ctx, `INSERT INTO state_documents(kind,id,owner_id,body) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING RETURNING kind,id,owner_id,body,version,created_at,updated_at`, d.Kind, d.ID, d.OwnerID, d.Body)
	created, err := scanDocument(row)
	if errors.Is(err, domain.ErrNotFound) {
		return d, domain.ErrConflict
	}
	if err != nil {
		return created, err
	}
	if err := appendEventsTx(ctx, tx, d.Events); err != nil {
		return d, err
	}
	if err := tx.Commit(ctx); err != nil {
		return d, err
	}
	return created, nil
}
func (p *Postgres) GetDocument(ctx context.Context, kind, id string) (domain.Document, error) {
	return scanDocument(p.pool.QueryRow(ctx, `SELECT kind,id,owner_id,body,version,created_at,updated_at FROM state_documents WHERE kind=$1 AND id=$2`, kind, id))
}
func (p *Postgres) UpdateDocument(ctx context.Context, d domain.Document, expected int64) (domain.Document, error) {
	tx, beginErr := p.pool.Begin(ctx)
	if beginErr != nil {
		return d, beginErr
	}
	defer tx.Rollback(ctx)
	row := tx.QueryRow(ctx, `UPDATE state_documents SET owner_id=$3,body=$4,version=version+1,updated_at=now() WHERE kind=$1 AND id=$2 AND version=$5 RETURNING kind,id,owner_id,body,version,created_at,updated_at`, d.Kind, d.ID, d.OwnerID, d.Body, expected)
	updated, err := scanDocument(row)
	if errors.Is(err, domain.ErrNotFound) {
		return d, domain.ErrConflict
	}
	if err != nil {
		return updated, err
	}
	if err := appendEventsTx(ctx, tx, d.Events); err != nil {
		return d, err
	}
	if err := tx.Commit(ctx); err != nil {
		return d, err
	}
	return updated, nil
}
func (p *Postgres) PutDocumentsAtomic(ctx context.Context, documents []domain.Document) ([]domain.Document, error) {
	tx, err := p.pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	out := make([]domain.Document, 0, len(documents))
	for _, doc := range documents {
		var stored domain.Document
		if doc.Version == 0 {
			stored, err = scanDocument(tx.QueryRow(ctx, `INSERT INTO state_documents(kind,id,owner_id,body) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING RETURNING kind,id,owner_id,body,version,created_at,updated_at`, doc.Kind, doc.ID, doc.OwnerID, doc.Body))
		} else {
			stored, err = scanDocument(tx.QueryRow(ctx, `UPDATE state_documents SET owner_id=$3,body=$4,version=version+1,updated_at=now() WHERE kind=$1 AND id=$2 AND version=$5 RETURNING kind,id,owner_id,body,version,created_at,updated_at`, doc.Kind, doc.ID, doc.OwnerID, doc.Body, doc.Version))
		}
		if errors.Is(err, domain.ErrNotFound) {
			return nil, domain.ErrConflict
		}
		if err != nil {
			return nil, err
		}
		if err := appendEventsTx(ctx, tx, doc.Events); err != nil {
			return nil, err
		}
		out = append(out, stored)
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return out, nil
}
func (p *Postgres) DeleteDocument(ctx context.Context, kind, id string, expected int64) error {
	tag, err := p.pool.Exec(ctx, `DELETE FROM state_documents WHERE kind=$1 AND id=$2 AND version=$3`, kind, id, expected)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrConflict
	}
	return nil
}
func (p *Postgres) DeleteSession(ctx context.Context, id string, expected int64, events []domain.Event) error {
	tx, err := p.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	tag, err := tx.Exec(ctx, `DELETE FROM state_documents WHERE kind='session' AND id=$1 AND version=$2`, id, expected)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrConflict
	}
	if err := appendEventsTx(ctx, tx, events); err != nil {
		return err
	}
	return tx.Commit(ctx)
}
func (p *Postgres) MigrateSession(ctx context.Context, oldDoc, newDoc domain.Document, expected int64) (domain.Document, domain.Document, error) {
	tx, err := p.pool.Begin(ctx)
	if err != nil {
		return oldDoc, newDoc, err
	}
	defer tx.Rollback(ctx)
	if err := appendEventsTx(ctx, tx, oldDoc.Events); err != nil {
		return oldDoc, newDoc, err
	}
	oldUpdated, err := scanDocument(tx.QueryRow(ctx, `UPDATE state_documents SET body=$3,version=version+1,updated_at=now() WHERE kind='session' AND id=$1 AND version=$2 RETURNING kind,id,owner_id,body,version,created_at,updated_at`, oldDoc.ID, expected, oldDoc.Body))
	if errors.Is(err, domain.ErrNotFound) {
		return oldDoc, newDoc, domain.ErrConflict
	}
	if err != nil {
		return oldDoc, newDoc, err
	}
	newCreated, err := scanDocument(tx.QueryRow(ctx, `INSERT INTO state_documents(kind,id,owner_id,body) VALUES('session',$1,$2,$3) ON CONFLICT DO NOTHING RETURNING kind,id,owner_id,body,version,created_at,updated_at`, newDoc.ID, newDoc.OwnerID, newDoc.Body))
	if errors.Is(err, domain.ErrNotFound) {
		return oldDoc, newDoc, domain.ErrConflict
	}
	if err != nil {
		return oldDoc, newDoc, err
	}
	if err := tx.Commit(ctx); err != nil {
		return oldDoc, newDoc, err
	}
	return oldUpdated, newCreated, nil
}
func appendEventsTx(ctx context.Context, tx pgx.Tx, events []domain.Event) error {
	for _, e := range events {
		if e.EventKey == "" {
			e.EventKey = domain.NewID("evt")
		}
		if _, err := tx.Exec(ctx, `INSERT INTO events(event_key,user_id,type,aggregate_type,aggregate_id,revision,correlation_id,payload) VALUES($1,$2,$3,$4,$5,$6,$7,$8)`, e.EventKey, e.UserID, e.Type, e.AggregateType, e.AggregateID, e.Revision, e.CorrelationID, e.Payload); err != nil {
			return err
		}
	}
	return nil
}
func (p *Postgres) ListDocuments(ctx context.Context, kind, owner string, limit int) ([]domain.Document, error) {
	rows, err := p.pool.Query(ctx, `SELECT kind,id,owner_id,body,version,created_at,updated_at FROM state_documents WHERE kind=$1 AND ($2='' OR owner_id=$2) ORDER BY updated_at DESC LIMIT $3`, kind, owner, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []domain.Document{}
	for rows.Next() {
		var d domain.Document
		if err := rows.Scan(&d.Kind, &d.ID, &d.OwnerID, &d.Body, &d.Version, &d.CreatedAt, &d.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}
func (p *Postgres) SearchSessions(ctx context.Context, q SessionQuery) ([]domain.Document, error) {
	contexts, _ := json.Marshal(q.Contexts)
	properties, _ := json.Marshal(q.Properties)
	member, _ := json.Marshal([]map[string]string{{"account_id": q.Requestor}})
	rows, err := p.pool.Query(ctx, `SELECT kind,id,owner_id,body,version,created_at,updated_at FROM state_documents WHERE kind='session' AND body->>'state'='open' AND (body->>'host_lease_expires_at')::timestamptz>now() AND body->>'title_id'=$1 AND body->>'media_id'=$2 AND body->>'title_version'=$3 AND (body->>'protocol_version')::integer=$4 AND body->'contexts' @> $5::jsonb AND body->'properties' @> $6::jsonb AND (body->>'visibility'<>'private' OR body->'members' @> $7::jsonb) AND jsonb_array_length(body->'members')<((body->>'public_slots')::integer+(body->>'private_slots')::integer) ORDER BY updated_at DESC LIMIT $8`, q.TitleID, q.MediaID, q.TitleVersion, q.ProtocolVersion, contexts, properties, member, q.Limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []domain.Document{}
	for rows.Next() {
		var d domain.Document
		if err := rows.Scan(&d.Kind, &d.ID, &d.OwnerID, &d.Body, &d.Version, &d.CreatedAt, &d.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}
func (p *Postgres) PutDevice(ctx context.Context, d domain.Device) error {
	tag, err := p.pool.Exec(ctx, `INSERT INTO devices(id,account_id,display_name,public_key,xuid,disabled,last_seen_at) VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT(id) DO UPDATE SET display_name=EXCLUDED.display_name,last_seen_at=EXCLUDED.last_seen_at WHERE devices.public_key=EXCLUDED.public_key`, d.ID, d.AccountID, d.DisplayName, d.PublicKey, d.XUID, d.Disabled, d.LastSeenAt)
	var databaseError *pgconn.PgError
	if errors.As(err, &databaseError) && databaseError.Code == "23505" {
		return domain.ErrConflict
	}
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrConflict
	}
	return nil
}
func (p *Postgres) GetDevice(ctx context.Context, id string) (domain.Device, error) {
	var d domain.Device
	err := p.pool.QueryRow(ctx, `SELECT id,account_id,display_name,public_key,xuid,disabled,created_at,last_seen_at FROM devices WHERE id=$1`, id).Scan(&d.ID, &d.AccountID, &d.DisplayName, &d.PublicKey, &d.XUID, &d.Disabled, &d.CreatedAt, &d.LastSeenAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return d, domain.ErrNotFound
	}
	return d, err
}
func (p *Postgres) GetDeviceByXUID(ctx context.Context, xuid string) (domain.Device, error) {
	var d domain.Device
	err := p.pool.QueryRow(ctx, `SELECT id,account_id,display_name,public_key,xuid,disabled,created_at,last_seen_at FROM devices WHERE xuid=$1`, xuid).Scan(&d.ID, &d.AccountID, &d.DisplayName, &d.PublicKey, &d.XUID, &d.Disabled, &d.CreatedAt, &d.LastSeenAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return d, domain.ErrNotFound
	}
	return d, err
}
func (p *Postgres) PutChallenge(ctx context.Context, c domain.Challenge) error {
	_, err := p.pool.Exec(ctx, `INSERT INTO device_challenges(id,device_id,nonce,xuid,public_key,expires_at,used) VALUES($1,$2,$3,$4,$5,$6,$7)`, c.ID, c.DeviceID, c.Nonce, c.XUID, c.PublicKey, c.ExpiresAt, c.Used)
	return err
}
func (p *Postgres) ConsumeChallenge(ctx context.Context, id, device string, now time.Time) (domain.Challenge, error) {
	var c domain.Challenge
	err := p.pool.QueryRow(ctx, `UPDATE device_challenges SET used=true WHERE id=$1 AND device_id=$2 AND used=false AND expires_at>$3 RETURNING id,device_id,nonce,xuid,public_key,expires_at,used`, id, device, now).Scan(&c.ID, &c.DeviceID, &c.Nonce, &c.XUID, &c.PublicKey, &c.ExpiresAt, &c.Used)
	if errors.Is(err, pgx.ErrNoRows) {
		return c, domain.ErrForbidden
	}
	return c, err
}
func (p *Postgres) PutRefresh(ctx context.Context, r domain.RefreshSession) error {
	_, err := p.pool.Exec(ctx, `INSERT INTO refresh_sessions(id,account_id,device_id,token_hash,expires_at,revoked) VALUES($1,$2,$3,$4,$5,$6)`, r.ID, r.AccountID, r.DeviceID, r.TokenHash, r.ExpiresAt, r.Revoked)
	return err
}
func (p *Postgres) ConsumeRefresh(ctx context.Context, hash string, now time.Time) (domain.RefreshSession, error) {
	var r domain.RefreshSession
	err := p.pool.QueryRow(ctx, `UPDATE refresh_sessions SET revoked=true WHERE token_hash=$1 AND revoked=false AND expires_at>$2 RETURNING id,account_id,device_id,token_hash,expires_at,revoked`, hash, now).Scan(&r.ID, &r.AccountID, &r.DeviceID, &r.TokenHash, &r.ExpiresAt, &r.Revoked)
	if errors.Is(err, pgx.ErrNoRows) {
		return r, domain.ErrForbidden
	}
	return r, err
}
func (p *Postgres) ReserveIdempotency(ctx context.Context, subject, method, path, keyValue, requestHash string, expires time.Time) (IdempotencyResult, IdempotencyState, error) {
	var inserted bool
	err := p.pool.QueryRow(ctx, `WITH reservation AS (
INSERT INTO idempotency_keys(subject,method,path,idempotency_key,request_hash,status,response_body,expires_at)
VALUES($1,$2,$3,$4,$5,0,'{}'::jsonb,$6)
ON CONFLICT(subject,method,path,idempotency_key) DO UPDATE
SET request_hash=EXCLUDED.request_hash,status=0,response_body='{}'::jsonb,expires_at=EXCLUDED.expires_at
WHERE idempotency_keys.expires_at<=now()
RETURNING true)
SELECT COALESCE((SELECT true FROM reservation),false)`, subject, method, path, keyValue, requestHash, expires).Scan(&inserted)
	if err != nil {
		return IdempotencyResult{}, IdempotencyConflict, err
	}
	if inserted {
		return IdempotencyResult{RequestHash: requestHash}, IdempotencyReserved, nil
	}
	var result IdempotencyResult
	err = p.pool.QueryRow(ctx, `SELECT status,response_body,request_hash FROM idempotency_keys WHERE subject=$1 AND method=$2 AND path=$3 AND idempotency_key=$4`, subject, method, path, keyValue).Scan(&result.Status, &result.Body, &result.RequestHash)
	if err != nil {
		return result, IdempotencyConflict, err
	}
	if result.RequestHash != requestHash {
		return result, IdempotencyConflict, nil
	}
	if result.Status == 0 {
		return result, IdempotencyInProgress, nil
	}
	return result, IdempotencyReplay, nil
}

func (p *Postgres) FinalizeIdempotency(ctx context.Context, subject, method, path, keyValue string, result IdempotencyResult, expires time.Time) error {
	command, err := p.pool.Exec(ctx, `UPDATE idempotency_keys SET status=$6,response_body=$7,expires_at=$8 WHERE subject=$1 AND method=$2 AND path=$3 AND idempotency_key=$4 AND request_hash=$5 AND status=0`, subject, method, path, keyValue, result.RequestHash, result.Status, result.Body, expires)
	if err != nil {
		return err
	}
	if command.RowsAffected() != 1 {
		return domain.ErrConflict
	}
	return nil
}
func (p *Postgres) AppendEvent(ctx context.Context, e domain.Event) (domain.Event, error) {
	if e.EventKey == "" {
		e.EventKey = domain.NewID("evt")
	}
	err := p.pool.QueryRow(ctx, `INSERT INTO events(event_key,user_id,type,aggregate_type,aggregate_id,revision,correlation_id,payload) VALUES($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id,created_at`, e.EventKey, e.UserID, e.Type, e.AggregateType, e.AggregateID, e.Revision, e.CorrelationID, e.Payload).Scan(&e.ID, &e.CreatedAt)
	return e, err
}
func (p *Postgres) AppendEvents(ctx context.Context, events []domain.Event) error {
	tx, err := p.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if err := appendEventsTx(ctx, tx, events); err != nil {
		return err
	}
	return tx.Commit(ctx)
}
func (p *Postgres) ReplayEvents(ctx context.Context, user string, after int64, limit int) ([]domain.Event, error) {
	rows, err := p.pool.Query(ctx, `SELECT id,event_key,user_id,type,aggregate_type,aggregate_id,revision,correlation_id,payload,created_at FROM events WHERE id>$1 AND (user_id=$2 OR user_id='*') ORDER BY id LIMIT $3`, after, user, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []domain.Event{}
	for rows.Next() {
		var e domain.Event
		if err := rows.Scan(&e.ID, &e.EventKey, &e.UserID, &e.Type, &e.AggregateType, &e.AggregateID, &e.Revision, &e.CorrelationID, &e.Payload, &e.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}
func (p *Postgres) AppendAudit(ctx context.Context, actor, action, targetType, targetID string, data []byte) error {
	if !json.Valid(data) {
		data = []byte(`{}`)
	}
	_, err := p.pool.Exec(ctx, `INSERT INTO audit_log(actor_id,action,target_type,target_id,data) VALUES($1,$2,$3,$4,$5)`, actor, action, targetType, targetID, data)
	return err
}
func (p *Postgres) LeaseOutbox(ctx context.Context, limit int) ([]domain.Event, error) {
	rows, err := p.pool.Query(ctx, `WITH candidates AS (SELECT id FROM events WHERE delivered_at IS NULL AND (claimed_until IS NULL OR claimed_until<now()) ORDER BY id FOR UPDATE SKIP LOCKED LIMIT $1) UPDATE events SET claimed_until=now()+interval '30 seconds' WHERE id IN (SELECT id FROM candidates) RETURNING id,event_key,user_id,type,aggregate_type,aggregate_id,revision,correlation_id,payload,created_at`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []domain.Event{}
	for rows.Next() {
		var e domain.Event
		if err := rows.Scan(&e.ID, &e.EventKey, &e.UserID, &e.Type, &e.AggregateType, &e.AggregateID, &e.Revision, &e.CorrelationID, &e.Payload, &e.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}
func (p *Postgres) MarkOutboxDelivered(ctx context.Context, ids []int64) error {
	_, err := p.pool.Exec(ctx, `UPDATE events SET delivered_at=now(),claimed_until=NULL WHERE id=ANY($1)`, ids)
	return err
}
