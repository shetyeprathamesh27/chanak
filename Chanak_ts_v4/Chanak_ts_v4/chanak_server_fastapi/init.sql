CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    email VARCHAR(255) UNIQUE NOT NULL,
    password VARCHAR(255) NOT NULL,
    name VARCHAR(255),
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS toys (
    id SERIAL PRIMARY KEY,
    tyid VARCHAR(20) UNIQUE NOT NULL,
    owner_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    pending_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    friendly_name VARCHAR(255),
    provision_token VARCHAR(255),
    device_token VARCHAR(255),
    provisioned BOOLEAN DEFAULT FALSE,
    reset_wifi_flag BOOLEAN DEFAULT FALSE,
    claim_expires_at TIMESTAMPTZ,
    claim_status VARCHAR(32),
    last_seen TIMESTAMPTZ,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS unclaimed_toys (
    id SERIAL PRIMARY KEY,
    tyid VARCHAR(20) UNIQUE NOT NULL
);

INSERT INTO unclaimed_toys (tyid) VALUES
    ('TY-A1B2C3D4'),
    ('TY-TEST001'),
    ('TY-TEST002')
ON CONFLICT (tyid) DO NOTHING;