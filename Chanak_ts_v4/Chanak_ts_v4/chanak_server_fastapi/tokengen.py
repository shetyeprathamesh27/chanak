import jwt
JWT_SECRET = "change-this-secret-key-please"  # same as your .env

token = jwt.encode(
    {"tyid": "TY-541967E6FC84", "user_id": 1, "type": "device"}, 
    JWT_SECRET, 
    algorithm="HS256"
)
print(token)