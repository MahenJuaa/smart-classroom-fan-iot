-- Smart Classroom Fan v3: Supabase PostgreSQL schema
-- Run this once in Supabase Dashboard > SQL Editor.

CREATE TABLE IF NOT EXISTS device_state (
    id SMALLINT PRIMARY KEY CHECK (id = 1),
    last_seen TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS classroom_state (
    class_id TEXT PRIMARY KEY CHECK (class_id IN ('A', 'B')),
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    motion BOOLEAN NOT NULL DEFAULT FALSE,
    fan_on BOOLEAN NOT NULL DEFAULT FALSE,
    desired_fan_on BOOLEAN NOT NULL DEFAULT FALSE,
    sensor_updated_at TIMESTAMPTZ,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT valid_temperature CHECK (temperature IS NULL OR temperature BETWEEN -20 AND 80),
    CONSTRAINT valid_humidity CHECK (humidity IS NULL OR humidity BETWEEN 0 AND 100)
);

CREATE TABLE IF NOT EXISTS sensor_logs (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    class_id TEXT NOT NULL REFERENCES classroom_state(class_id) ON DELETE CASCADE,
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    motion BOOLEAN NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS fan_commands (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    class_id TEXT NOT NULL REFERENCES classroom_state(class_id) ON DELETE CASCADE,
    desired_fan_on BOOLEAN NOT NULL,
    source TEXT NOT NULL DEFAULT 'dashboard' CHECK (source IN ('dashboard')),
    status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'applied', 'superseded')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    applied_at TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS fan_logs (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    class_id TEXT NOT NULL REFERENCES classroom_state(class_id) ON DELETE CASCADE,
    fan_on BOOLEAN NOT NULL,
    source TEXT NOT NULL CHECK (source IN ('dashboard', 'physical_button', 'device_sync')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_sensor_logs_class_created ON sensor_logs (class_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_fan_logs_class_created ON fan_logs (class_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_fan_commands_class_status ON fan_commands (class_id, status, id DESC);

INSERT INTO device_state (id) VALUES (1) ON CONFLICT (id) DO NOTHING;
INSERT INTO classroom_state (class_id) VALUES ('A'), ('B') ON CONFLICT (class_id) DO NOTHING;

-- Tables are accessed through the authenticated backend connection only.
-- Do not expose the Supabase secret key or database password in ESP32/browser code.
ALTER TABLE device_state ENABLE ROW LEVEL SECURITY;
ALTER TABLE classroom_state ENABLE ROW LEVEL SECURITY;
ALTER TABLE sensor_logs ENABLE ROW LEVEL SECURITY;
ALTER TABLE fan_commands ENABLE ROW LEVEL SECURITY;
ALTER TABLE fan_logs ENABLE ROW LEVEL SECURITY;

REVOKE ALL ON TABLE device_state, classroom_state, sensor_logs, fan_commands, fan_logs FROM PUBLIC, anon, authenticated;
REVOKE ALL ON SEQUENCE sensor_logs_id_seq, fan_commands_id_seq, fan_logs_id_seq FROM PUBLIC, anon, authenticated;
