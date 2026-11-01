-- LiveStreamLab N01 / schema version 1.
-- Target: Oracle MySQL 8.0.16+ (InnoDB, enforced CHECK constraints).
-- Select a NEW, dedicated schema before applying this file. No CREATE DATABASE,
-- USE, DROP, account changes, data seeds, or automatic migration are performed.
-- MySQL DDL implicitly commits: this file is deliberately not wrapped in a
-- transaction and must not be blindly rerun after a partial failure.
-- All DATETIME values are UTC. Application writes bind UTC values explicitly.
SET NAMES utf8mb4;
SET time_zone = '+00:00';

CREATE TABLE schema_migrations (
    version INT UNSIGNED NOT NULL,
    description VARCHAR(120) NOT NULL,
    applied_at DATETIME(6) NOT NULL,
    PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE users (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    nickname VARCHAR(32) NOT NULL,
    password_hash VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    created_at DATETIME(6) NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_users_username (username),
    CONSTRAINT ck_users_username CHECK (
        REGEXP_LIKE(username, _ascii'^[a-z0-9_]{3,32}$', 'c')
    ),
    CONSTRAINT ck_users_nickname CHECK (CHAR_LENGTH(TRIM(nickname)) BETWEEN 1 AND 32),
    CONSTRAINT ck_users_password_hash CHECK (password_hash LIKE '$argon2id$%')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE auth_sessions (
    id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    token_hash BINARY(32) NOT NULL,
    created_at DATETIME(6) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    revoked_at DATETIME(6) NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_auth_sessions_token_hash (token_hash),
    KEY ix_auth_sessions_user_expiry (user_id, expires_at),
    KEY ix_auth_sessions_expiry (expires_at),
    CONSTRAINT fk_auth_sessions_user FOREIGN KEY (user_id) REFERENCES users (id),
    CONSTRAINT ck_auth_sessions_expiry CHECK (expires_at > created_at),
    CONSTRAINT ck_auth_sessions_revoked CHECK (revoked_at IS NULL OR revoked_at >= created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE rooms (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    owner_id BIGINT UNSIGNED NOT NULL,
    title VARCHAR(120) NOT NULL,
    category VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'general',
    current_session_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    created_at DATETIME(6) NOT NULL,
    updated_at DATETIME(6) NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_rooms_owner (owner_id),
    KEY ix_rooms_current_session (id, current_session_id),
    CONSTRAINT fk_rooms_owner FOREIGN KEY (owner_id) REFERENCES users (id),
    CONSTRAINT ck_rooms_title CHECK (CHAR_LENGTH(TRIM(title)) BETWEEN 1 AND 120),
    CONSTRAINT ck_rooms_category CHECK (category IN ('general', 'gaming', 'music', 'education', 'technology')),
    CONSTRAINT ck_rooms_updated CHECK (updated_at >= created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE live_sessions (
    id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    room_id BIGINT UNSIGNED NOT NULL,
    client_request_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    stream_name CHAR(34) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    publish_token_hash BINARY(32) NULL,
    status VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'PREPARING',
    allow_publish TINYINT UNSIGNED NOT NULL DEFAULT 1,
    created_at DATETIME(6) NOT NULL,
    prepare_deadline DATETIME(6) NOT NULL,
    started_at DATETIME(6) NULL,
    ended_at DATETIME(6) NULL,
    reconnect_deadline DATETIME(6) NULL,
    end_reason VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,

    -- Normalized SRS identities, never source IP. Retain the last connection
    -- during reconnect so an old unpublish event cannot affect a replacement.
    srs_server_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
    srs_service_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
    srs_stream_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
    srs_client_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
    publish_generation BIGINT UNSIGNED NOT NULL DEFAULT 0,
    revision BIGINT UNSIGNED NOT NULL DEFAULT 0,
    last_confirmed_at DATETIME(6) NULL,
    last_media_progress_at DATETIME(6) NULL,
    srs_recv_bytes BIGINT UNSIGNED NULL,
    srs_audio_frames BIGINT UNSIGNED NULL,
    srs_video_frames BIGINT UNSIGNED NULL,

    -- MySQL permits several NULLs in a UNIQUE index. Terminal history remains
    -- unlimited while every room has at most one nonterminal session.
    active_room_id BIGINT UNSIGNED GENERATED ALWAYS AS (
        CASE WHEN status IN ('PREPARING', 'LIVE', 'RECONNECTING', 'STOPPING')
             THEN room_id ELSE NULL END
    ) STORED,
    PRIMARY KEY (id),
    UNIQUE KEY uq_live_sessions_room_id (room_id, id),
    UNIQUE KEY uq_live_sessions_room_request (room_id, client_request_id),
    UNIQUE KEY uq_live_sessions_stream_name (stream_name),
    UNIQUE KEY uq_live_sessions_active_room (active_room_id),
    KEY ix_live_sessions_lobby (status, started_at DESC, room_id DESC),
    KEY ix_live_sessions_reconcile (status, last_confirmed_at),
    KEY ix_live_sessions_prepare_timeout (status, prepare_deadline),
    KEY ix_live_sessions_reconnect_timeout (status, reconnect_deadline),
    CONSTRAINT fk_live_sessions_room FOREIGN KEY (room_id) REFERENCES rooms (id),
    CONSTRAINT ck_live_sessions_status CHECK (
        status IN ('PREPARING', 'LIVE', 'RECONNECTING', 'STOPPING', 'ENDED', 'FAILED')
    ),
    CONSTRAINT ck_live_sessions_publish_permission CHECK (
        (status IN ('PREPARING', 'LIVE', 'RECONNECTING') AND allow_publish = 1 AND publish_token_hash IS NOT NULL)
        OR (status IN ('STOPPING', 'ENDED', 'FAILED') AND allow_publish = 0 AND publish_token_hash IS NULL)
    ),
    CONSTRAINT ck_live_sessions_prepare_deadline CHECK (prepare_deadline > created_at),
    CONSTRAINT ck_live_sessions_end_reason CHECK (
        end_reason IS NULL
        OR end_reason IN ('OWNER_STOPPED', 'PREPARE_TIMEOUT', 'RECONNECT_TIMEOUT', 'PUBLISH_FAILED')
    ),
    CONSTRAINT ck_live_sessions_start_time CHECK (started_at IS NULL OR started_at >= created_at),
    CONSTRAINT ck_live_sessions_end_time CHECK (ended_at IS NULL OR ended_at >= created_at),
    CONSTRAINT ck_live_sessions_end_after_start CHECK (
        ended_at IS NULL OR started_at IS NULL OR ended_at >= started_at
    ),
    CONSTRAINT ck_live_sessions_terminal_time CHECK (
        (status IN ('ENDED', 'FAILED') AND ended_at IS NOT NULL)
        OR (status NOT IN ('ENDED', 'FAILED') AND ended_at IS NULL)
    ),
    CONSTRAINT ck_live_sessions_reconnect CHECK (
        (status = 'RECONNECTING' AND started_at IS NOT NULL
            AND reconnect_deadline IS NOT NULL AND reconnect_deadline >= created_at)
        OR (status <> 'RECONNECTING' AND reconnect_deadline IS NULL)
    ),
    CONSTRAINT ck_live_sessions_srs_identity CHECK (
        (srs_server_id IS NULL AND srs_service_id IS NULL AND srs_stream_id IS NULL AND srs_client_id IS NULL)
        OR (srs_server_id IS NOT NULL AND srs_service_id IS NOT NULL AND srs_client_id IS NOT NULL)
    ),
    CONSTRAINT ck_live_sessions_media_ready CHECK (
        status <> 'LIVE'
        OR (started_at IS NOT NULL AND last_media_progress_at IS NOT NULL
            AND srs_server_id IS NOT NULL AND srs_service_id IS NOT NULL
            AND srs_stream_id IS NOT NULL AND srs_client_id IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- Added after both tables exist; nullable current_session_id permits initial
-- room creation. The composite FK prevents pointing a room at another room's
-- session. No cascades: stop/revoke and pointer clearing are explicit actions.
ALTER TABLE rooms ADD CONSTRAINT fk_rooms_current_session
    FOREIGN KEY (id, current_session_id) REFERENCES live_sessions (room_id, id);

INSERT INTO schema_migrations (version, description, applied_at)
VALUES (1, 'N01 users, auth sessions, rooms and live sessions', UTC_TIMESTAMP(6));
