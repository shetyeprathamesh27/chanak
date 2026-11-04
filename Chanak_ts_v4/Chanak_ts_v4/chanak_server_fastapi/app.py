import os
import jwt
import bcrypt
import psycopg2
from psycopg2.extras import RealDictCursor
from datetime import datetime, timedelta, timezone
from fastapi import FastAPI, HTTPException, Depends, status, APIRouter
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from pydantic import BaseModel, EmailStr, Field
from typing import Optional
import secrets
from pathlib import Path

DATABASE_URL = os.getenv("DATABASE_URL", "postgresql://postgres:admin@localhost:5432/chanak")
JWT_SECRET = os.getenv("JWT_SECRET", "change-this-secret-key-please")
JWT_ALGORITHM = "HS256"
FRONTEND_DIR = Path(r"D:\Chanak\Chanak_ts_v4\Chanak_ts_v4\chanak_server_fastapi\templates")
CLAIM_TIMEOUT_MINUTES = 10
ACCESS_TOKEN_EXPIRE_DAYS = 30
app = FastAPI(title="Chanak Toy Management API")
api = APIRouter(prefix="/api")
security = HTTPBearer()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

def get_db():
    conn = psycopg2.connect(DATABASE_URL, cursor_factory=RealDictCursor)
    try:
        yield conn
    finally:
        conn.close()

class RegisterRequest(BaseModel):
    email: EmailStr
    password: str = Field(min_length=8)
    name: Optional[str] = None

class LoginRequest(BaseModel):
    email: EmailStr
    password: str

class TokenResponse(BaseModel):
    token: str

class ClaimStartRequest(BaseModel):
    tyid: str

class ClaimStartResponse(BaseModel):
    status: str
    tyid: Optional[str] = None
    ap_ssid: Optional[str] = None

class ClaimConfirmRequest(BaseModel):
    tyid: str

class ToyInfo(BaseModel):
    tyid: str
    friendly_name: str
    online: bool
    last_seen: Optional[datetime]

class MyToysResponse(BaseModel):
    toys: list[ToyInfo]

class UpdateToyRequest(BaseModel):
    friendly_name: Optional[str] = None

class HeartbeatRequest(BaseModel):
    tyid: str

class ProvisionAckRequest(BaseModel):
    tyid: str
    token: str

def hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode('utf-8'), bcrypt.gensalt()).decode('utf-8')

def verify_password(plain_password: str, hashed_password: str) -> bool:
    return bcrypt.checkpw(plain_password.encode('utf-8'), hashed_password.encode('utf-8'))

def create_access_token(data: dict) -> str:
    to_encode = data.copy()
    expire = datetime.now(timezone.utc) + timedelta(days=ACCESS_TOKEN_EXPIRE_DAYS)
    to_encode.update({"exp": expire})
    return jwt.encode(to_encode, JWT_SECRET, algorithm=JWT_ALGORITHM)

def create_device_token(tyid: str, user_id: int) -> str:
    payload = {"tyid": tyid, "user_id": user_id, "type": "device"}
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)

def get_current_user(credentials: HTTPAuthorizationCredentials = Depends(security), conn = Depends(get_db)) -> dict:
    token = credentials.credentials
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        user_id: int = int(payload.get("sub"))
    except:
        raise HTTPException(status_code=401, detail="Invalid token")

    cur = conn.cursor()
    cur.execute("SELECT id, email, name FROM users WHERE id = %s", (user_id,))
    user = cur.fetchone()
    cur.close()
    if user is None:
        raise HTTPException(status_code=401, detail="User not found")
    return user

@api.post("/register", status_code=201)
def register(req: RegisterRequest, conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("SELECT id FROM users WHERE email = %s", (req.email,))
    if cur.fetchone():
        cur.close()
        raise HTTPException(status_code=400, detail="Email already registered")

    hashed = hash_password(req.password)
    cur.execute(
        "INSERT INTO users (email, password, name) VALUES (%s, %s, %s)",
        (req.email, hashed, req.name or "")
    )
    conn.commit()
    cur.close()
    return {"message": "User created"}

@api.post("/login", response_model=TokenResponse)
def login(req: LoginRequest, conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("SELECT id, password FROM users WHERE email = %s", (req.email,))
    user = cur.fetchone()
    cur.close()
    if not user or not verify_password(req.password, user["password"]):
        raise HTTPException(status_code=401, detail="Invalid credentials")
    token = create_access_token({"sub": str(user["id"])})
    return {"token": token}

@api.get("/my_toys", response_model=MyToysResponse)
def get_my_toys(current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("SELECT tyid, friendly_name, last_seen FROM toys WHERE owner_id = %s AND provisioned = TRUE", (current_user["id"],))
    toys = cur.fetchall()
    cur.close()

    now = datetime.now(timezone.utc)
    result = []
    for t in toys:
        last_seen = t["last_seen"]
        online = False
        if last_seen:
            if last_seen.tzinfo is None:
                last_seen = last_seen.replace(tzinfo=timezone.utc)
            online = (now - last_seen) < timedelta(minutes=2)
        result.append(ToyInfo(
            tyid=t["tyid"],
            friendly_name=t.get("friendly_name") or t["tyid"],
            online=online,
            last_seen=last_seen
        ))
    return {"toys": result}

@api.post("/claim/start", response_model=ClaimStartResponse)
def claim_start(req: ClaimStartRequest, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    tyid = req.tyid.upper().strip()
    cur = conn.cursor()

    cur.execute("SELECT id, owner_id, provisioned FROM toys WHERE tyid = %s", (tyid,))
    existing = cur.fetchone()
    if existing and existing['provisioned']:
        cur.close()
        return ClaimStartResponse(status="claimed")

    cur.execute("SELECT id FROM unclaimed_toys WHERE tyid = %s", (tyid,))
    if not cur.fetchone():
        cur.close()
        raise HTTPException(status_code=404, detail="Invalid TYID")

    expires_at = datetime.now(timezone.utc) + timedelta(minutes=CLAIM_TIMEOUT_MINUTES)

    if existing:
        cur.execute("""
            UPDATE toys SET pending_user_id = %s, claim_expires_at = %s, claim_status = 'pending'
            WHERE tyid = %s AND provisioned = FALSE
        """, (current_user["id"], expires_at, tyid))
    else:
        cur.execute("""
            INSERT INTO toys (tyid, pending_user_id, friendly_name, claim_expires_at, claim_status, provisioned)
            VALUES (%s, %s, %s, %s, 'pending', FALSE)
        """, (tyid, current_user["id"], tyid, expires_at))

    conn.commit()
    cur.close()

    return ClaimStartResponse(
        status="ready",
        tyid=tyid,
        ap_ssid=f"Chanak-{tyid}"
    )

@api.get("/device/status")
def device_status(tyid: str, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    tyid = tyid.upper().strip()
    cur = conn.cursor()
    cur.execute("""
        SELECT provisioned, claim_status, claim_expires_at, last_seen, pending_user_id
        FROM toys WHERE tyid = %s
    """, (tyid,))
    toy = cur.fetchone()
    cur.close()

    if not toy:
        return {"status": "offline"}

    expires_at = toy["claim_expires_at"]
    if expires_at:
        if expires_at.tzinfo is None:
            expires_at = expires_at.replace(tzinfo=timezone.utc)
        if expires_at < datetime.now(timezone.utc) and not toy["provisioned"]:
            return {"status": "expired"}

    if toy["provisioned"]:
        return {"status": "provisioned"}

    last_seen = toy["last_seen"]
    if last_seen:
        if last_seen.tzinfo is None:
            last_seen = last_seen.replace(tzinfo=timezone.utc)
        if (datetime.now(timezone.utc) - last_seen) < timedelta(seconds=30):
            return {"status": "waiting_for_claim"}

    return {"status": "offline"}

@api.post("/claim/confirm")
def claim_confirm(req: ClaimConfirmRequest, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    tyid = req.tyid.upper().strip()
    cur = conn.cursor()

    cur.execute("""
        SELECT id, pending_user_id, claim_expires_at, provisioned
        FROM toys WHERE tyid = %s
    """, (tyid,))
    toy = cur.fetchone()

    if not toy:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")

    if toy["provisioned"]:
        cur.close()
        raise HTTPException(status_code=409, detail="Already claimed")

    if toy["pending_user_id"]!= current_user["id"]:
        cur.close()
        raise HTTPException(status_code=403, detail="Not your claim")

    expires_at = toy["claim_expires_at"]
    if expires_at:
        if expires_at.tzinfo is None:
            expires_at = expires_at.replace(tzinfo=timezone.utc)
        if expires_at < datetime.now(timezone.utc):
            cur.close()
            raise HTTPException(status_code=410, detail="Claim expired")

    device_token = create_device_token(tyid, current_user["id"])

    cur.execute("""
        UPDATE toys SET device_token = %s, claim_status = 'confirmed', owner_id = %s
        WHERE tyid = %s
    """, (device_token, current_user["id"], tyid))

    conn.commit()
    cur.close()
    return {"status": "ok"}

# FIXED: Removed /api prefix - router already adds it
@api.get("/device/wait_for_claim")
def wait_for_claim(tyid: str, conn = Depends(get_db)):
    tyid = tyid.upper().strip()
    print(f"wait_for_claim: tyid={tyid}")

    cur = conn.cursor()
    cur.execute("UPDATE toys SET last_seen = %s WHERE tyid = %s",
                (datetime.now(timezone.utc), tyid))
    if cur.rowcount == 0:
        cur.close()
        print(f"No toy row for {tyid}")
        raise HTTPException(status_code=404, detail="No claim")
    conn.commit()

    cur.execute("""
        SELECT claim_status, device_token, claim_expires_at, provisioned
        FROM toys WHERE tyid = %s
    """, (tyid,))
    toy = cur.fetchone()

    if not toy:
        cur.close()
        raise HTTPException(status_code=404, detail="No claim")

    print(f"Found toy: status={toy['claim_status']}, provisioned={toy['provisioned']}")

    if toy["provisioned"]:
        cur.close()
        raise HTTPException(status_code=404, detail="Already provisioned")

    expires_at = toy["claim_expires_at"]
    if expires_at:
        if expires_at.tzinfo is None:
            expires_at = expires_at.replace(tzinfo=timezone.utc)
        if expires_at < datetime.now(timezone.utc):
            cur.close()
            print(f"Claim expired for {tyid}")
            raise HTTPException(status_code=404, detail="Claim expired")

    if toy["claim_status"] == "confirmed" and toy["device_token"]:
        cur.close()
        return {"token": toy["device_token"], "tyid": tyid}

    cur.close()
    print(f"Returning 204 for {tyid} - keep polling")
    raise HTTPException(status_code=204)

@api.post("/provision/ack")
def provision_ack(req: ProvisionAckRequest, conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("""
        SELECT id FROM toys
        WHERE tyid = %s AND device_token = %s AND provisioned = FALSE
    """, (req.tyid, req.token))
    toy = cur.fetchone()
    if not toy:
        cur.close()
        raise HTTPException(status_code=400, detail="Invalid token or TYID")

    cur.execute("DELETE FROM unclaimed_toys WHERE tyid = %s", (req.tyid,))
    cur.execute("""
        UPDATE toys SET provisioned = TRUE, last_seen = %s, claim_status = NULL, claim_expires_at = NULL
        WHERE id = %s
    """, (datetime.now(timezone.utc), toy["id"]))
    conn.commit()
    cur.close()
    return {"status": "ok"}

@api.get("/device/validate")
def validate_device(credentials: HTTPAuthorizationCredentials = Depends(security), conn = Depends(get_db)):
    token = credentials.credentials
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        if payload.get("type")!= "device":
            raise HTTPException(status_code=401, detail="Not a device token")
        tyid = payload.get("tyid")
    except:
        raise HTTPException(status_code=401, detail="Invalid token")

    cur = conn.cursor()
    cur.execute("SELECT id FROM toys WHERE tyid = %s AND provisioned = TRUE", (tyid,))
    if not cur.fetchone():
        cur.close()
        raise HTTPException(status_code=401, detail="Device deleted")
    cur.close()
    return {"status": "ok"}

@api.post("/heartbeat")
def heartbeat(req: HeartbeatRequest, conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("UPDATE toys SET last_seen = %s WHERE tyid = %s", (datetime.now(timezone.utc), req.tyid))
    if cur.rowcount == 0:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")
    conn.commit()
    cur.close()
    return {"status": "ok"}

@api.get("/toy/status/{tyid}")
def toy_status_legacy(tyid: str, conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("SELECT id, reset_wifi_flag FROM toys WHERE tyid = %s", (tyid,))
    toy = cur.fetchone()
    if not toy:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")

    resp = {}
    if toy["reset_wifi_flag"]:
        resp["reset_wifi"] = True
        cur.execute("UPDATE toys SET reset_wifi_flag = FALSE WHERE id = %s", (toy["id"],))
        conn.commit()
    cur.close()
    return resp

@api.patch("/toys/{tyid}")
def update_toy(tyid: str, req: UpdateToyRequest, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    if not req.friendly_name:
        raise HTTPException(status_code=400, detail="No data to update")

    cur = conn.cursor()
    cur.execute("UPDATE toys SET friendly_name = %s WHERE tyid = %s AND owner_id = %s",
                (req.friendly_name, tyid, current_user["id"]))
    if cur.rowcount == 0:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")
    conn.commit()
    cur.close()
    return {"status": "ok"}

@api.delete("/toys/{tyid}")
def delete_toy(tyid: str, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("DELETE FROM toys WHERE tyid = %s AND owner_id = %s RETURNING id", (tyid, current_user["id"]))
    if cur.rowcount == 0:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")

    cur.execute("INSERT INTO unclaimed_toys (tyid) VALUES (%s) ON CONFLICT (tyid) DO NOTHING", (tyid,))
    conn.commit()
    cur.close()
    return {"status": "deleted"}

@api.post("/toy/reset_wifi/{tyid}")
def reset_wifi(tyid: str, current_user: dict = Depends(get_current_user), conn = Depends(get_db)):
    cur = conn.cursor()
    cur.execute("UPDATE toys SET reset_wifi_flag = TRUE WHERE tyid = %s AND owner_id = %s",
                (tyid, current_user["id"]))
    if cur.rowcount == 0:
        cur.close()
        raise HTTPException(status_code=404, detail="Toy not found")
    conn.commit()
    cur.close()
    return {"status": "flag_set"}

app.include_router(api)
app.mount("/static", StaticFiles(directory=FRONTEND_DIR), name="static")

@app.get("/")
async def serve_index():
    return FileResponse(FRONTEND_DIR / 'index.html')

@app.get("/health")
def health():
    return {"ok": True}