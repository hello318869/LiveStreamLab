-- N03: differentiate a newly accepted publisher from a disconnected publisher.
-- Apply only after schema version 1, to the same dedicated database.
-- Existing rows are reconciled against SRS before new publish work is accepted.
ALTER TABLE live_sessions
    ADD COLUMN publish_connected TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER publish_generation,
    ADD COLUMN publish_accepted_at DATETIME(6) NULL AFTER publish_connected,
    ADD CONSTRAINT ck_live_sessions_connected CHECK (publish_connected IN (0, 1));

INSERT INTO schema_migrations (version, description, applied_at)
VALUES (2, 'N03 track accepted publish connection', UTC_TIMESTAMP(6));
