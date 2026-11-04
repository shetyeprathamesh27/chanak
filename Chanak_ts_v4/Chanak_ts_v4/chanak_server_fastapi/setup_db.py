import psycopg2
from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT

# 1. Create database
conn = psycopg2.connect("postgresql://postgres:admin@localhost:5432/postgres")
conn.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
cur = conn.cursor()
cur.execute("CREATE DATABASE chanak;")
cur.close()
conn.close()
print("Created database 'chanak'")

# 2. Connect to new DB and create tables
conn = psycopg2.connect("postgresql://postgres:admin@localhost:5432/chanak")
cur = conn.cursor()

print("Creating tables...")

cur.execute("""
    CREATE TABLE users (
        id SERIAL PRIMARY KEY,
        email VARCHAR(255) UNIQUE NOT NULL,
        password VARCHAR(255) NOT NULL,
        name VARCHAR(255),
        created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
    );
""")

cur.execute("""
    CREATE TABLE toys (
        id SERIAL PRIMARY KEY,
        tyid VARCHAR(20) UNIQUE NOT NULL,
        owner_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
        friendly_name VARCHAR(255),
        provision_token VARCHAR(255),
        provisioned BOOLEAN DEFAULT FALSE,
        reset_wifi_flag BOOLEAN DEFAULT FALSE,
        last_seen TIMESTAMP WITH TIME ZONE,
        created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
    );
""")

cur.execute("""
    CREATE TABLE unclaimed_toys (
        id SERIAL PRIMARY KEY,
        tyid VARCHAR(20) UNIQUE NOT NULL
    );
""")

# 3. Insert test TYIDs for development
cur.execute("""
    INSERT INTO unclaimed_toys (tyid) VALUES
    ('TY-A1B2C3D4'),
    ('TY-TEST001'),
    ('TY-TEST002');
""")

conn.commit()

# 4. Verify
cur.execute("SELECT table_name FROM information_schema.tables WHERE table_schema = 'public'")
tables = cur.fetchall()
print(f"\nTables created: {[t[0] for t in tables]}")

cur.execute("SELECT tyid FROM unclaimed_toys")
tyids = cur.fetchall()
print(f"Unclaimed TYIDs: {[t[0] for t in tyids]}")

cur.close()
conn.close()
print("\nDB setup complete. Ready to run server.")