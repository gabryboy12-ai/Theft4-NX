CREATE TABLE IF NOT EXISTS schema_migrations(version text PRIMARY KEY, applied_at timestamptz NOT NULL DEFAULT now());

CREATE TABLE IF NOT EXISTS devices(
  id text PRIMARY KEY, account_id text NOT NULL, display_name text NOT NULL,
  public_key text NOT NULL, xuid text NOT NULL, disabled boolean NOT NULL DEFAULT false,
  created_at timestamptz NOT NULL DEFAULT now(), last_seen_at timestamptz NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX IF NOT EXISTS devices_xuid_idx ON devices(xuid);

CREATE TABLE IF NOT EXISTS device_challenges(
  id text PRIMARY KEY, device_id text NOT NULL, nonce text NOT NULL, xuid text NOT NULL,
  public_key text NOT NULL, expires_at timestamptz NOT NULL, used boolean NOT NULL DEFAULT false
);
CREATE INDEX IF NOT EXISTS device_challenges_expiry_idx ON device_challenges(expires_at);

CREATE TABLE IF NOT EXISTS refresh_sessions(
  id text PRIMARY KEY, account_id text NOT NULL, device_id text NOT NULL REFERENCES devices(id),
  token_hash text UNIQUE NOT NULL, expires_at timestamptz NOT NULL, revoked boolean NOT NULL DEFAULT false
);

CREATE TABLE IF NOT EXISTS state_documents(
  kind text NOT NULL, id text NOT NULL, owner_id text NOT NULL, body jsonb NOT NULL,
  version bigint NOT NULL DEFAULT 1 CHECK(version > 0), created_at timestamptz NOT NULL DEFAULT now(),
  updated_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(kind,id)
);
CREATE INDEX IF NOT EXISTS state_documents_owner_idx ON state_documents(kind,owner_id,updated_at DESC);
CREATE INDEX IF NOT EXISTS state_documents_body_idx ON state_documents USING gin(body);
CREATE INDEX IF NOT EXISTS sessions_compatibility_idx ON state_documents((body->>'title_id'),(body->>'media_id'),(body->>'title_version'),((body->>'protocol_version')::integer),updated_at DESC) WHERE kind='session' AND body->>'state'='open';
CREATE INDEX IF NOT EXISTS sessions_contexts_idx ON state_documents USING gin((body->'contexts')) WHERE kind='session';
CREATE INDEX IF NOT EXISTS sessions_properties_idx ON state_documents USING gin((body->'properties')) WHERE kind='session';

CREATE TABLE IF NOT EXISTS events(
  id bigserial PRIMARY KEY, event_key text UNIQUE NOT NULL, user_id text NOT NULL,
  type text NOT NULL, aggregate_type text NOT NULL, aggregate_id text NOT NULL,
  revision bigint NOT NULL, correlation_id text NOT NULL DEFAULT '', payload jsonb NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now(), delivered_at timestamptz, claimed_until timestamptz
);
CREATE INDEX IF NOT EXISTS events_replay_idx ON events(user_id,id);
CREATE INDEX IF NOT EXISTS events_outbox_idx ON events(id) WHERE delivered_at IS NULL;

CREATE TABLE IF NOT EXISTS idempotency_keys(
  subject text NOT NULL, method text NOT NULL, path text NOT NULL, idempotency_key text NOT NULL, request_hash text NOT NULL,
  status integer NOT NULL, response_body jsonb NOT NULL, expires_at timestamptz NOT NULL,
  PRIMARY KEY(subject,method,path,idempotency_key)
);

CREATE TABLE IF NOT EXISTS audit_log(
  id bigserial PRIMARY KEY, actor_id text NOT NULL, action text NOT NULL, target_type text NOT NULL,
  target_id text NOT NULL, data jsonb NOT NULL, created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS audit_log_target_idx ON audit_log(target_type,target_id,created_at DESC);

-- Normalized durable tables reserved for projection workers and analytics. The API writes its
-- transaction-safe source documents and immutable event/outbox records first.
CREATE TABLE IF NOT EXISTS match_result_ledger(
  id text PRIMARY KEY, session_id text NOT NULL, submitter_id text NOT NULL, result jsonb NOT NULL,
  status text NOT NULL DEFAULT 'quarantined', created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS progression_totals(account_id text PRIMARY KEY, cash bigint NOT NULL DEFAULT 0, rank integer NOT NULL DEFAULT 0, revision bigint NOT NULL DEFAULT 1, updated_at timestamptz NOT NULL DEFAULT now());
CREATE TABLE IF NOT EXISTS leaderboard_entries(board text NOT NULL, season text NOT NULL, account_id text NOT NULL, score bigint NOT NULL, payload jsonb NOT NULL DEFAULT '{}', PRIMARY KEY(board,season,account_id));
CREATE TABLE IF NOT EXISTS moderation_actions(id text PRIMARY KEY, subject_id text NOT NULL, moderator_id text NOT NULL, action text NOT NULL, reason text NOT NULL, expires_at timestamptz, created_at timestamptz NOT NULL DEFAULT now());
