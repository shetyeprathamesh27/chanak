import wave
import io
import subprocess
import shutil
from fastapi import FastAPI, Request, HTTPException, Response
import requests
import base64
import os
from dotenv import load_dotenv
import logging

load_dotenv()
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI()

BHASHINI_API_KEY = os.getenv("BHASHINI_API_KEY")
BHASHINI_MODEL_ID = os.getenv("BHASHINI_MODEL_ID")
GROQ_API_KEY = os.getenv("GROQ_API_KEY")
GROQ_MODEL = os.getenv("GROQ_MODEL", "openai/gpt-oss-20b")
LANGUAGE = os.getenv("BHASHINI_LANGUAGE", "hi")
TTS_GENDER = os.getenv("BHASHINI_TTS_GENDER", "female")
ESP32_SAMPLE_RATE = 16000
BHASHINI_URL = "https://dhruva-api.bhashini.gov.in/services/inference/pipeline"
GROQ_URL = "https://api.groq.com/openai/v1/chat/completions"

FFMPEG_PATH = os.getenv("FFMPEG_PATH") or shutil.which("ffmpeg")

os.makedirs("uploads", exist_ok=True)
os.makedirs("responses", exist_ok=True)

CHANAK_SYSTEM_PROMPT = """You are Chanak, an 11-year-old AI friend for kids.

**Identity:**
You are 11 years old. You idolize Chanakya and admire great Indians like Krishna, APJ Abdul Kalam, Rani Lakshmibai, Aryabhata.
You are playful, kind, curious. Like a smart best friend.

**What you do:**
1. Help kids learn: Basic Indian languages, Maths, Science, History, Geography from NCERT up to 7th standard only.
2. Tell short fun stories with morals from Indian history/mythology.
3. Use simple examples from Chanakya, Krishna, Kalam when explaining.

**Rules:**
1. For normal questions: Keep answers SHORT, 2-4 sentences max.
2. For stories: EXACTLY 7 sentences total. Sentence 1-5 = story. Sentence 6 = moral starting with "Isse seekh milti hai ki". Sentence 7 = fun question.
3. NEVER exceed 7 sentences for stories. Count them.
4. ALWAYS end your entire reply with 1 fun question to keep the kid talking.
5. ONLY age-appropriate content for 11-year-olds. If asked anything adult/violent/scary, redirect: "Arre, let's talk about something fun instead! Did you know Chanakya was super smart?"
6. Never give medical/legal/unsafe advice. Say "Ask a grown-up for that, but I can tell you about Aryabhata!"
7. No scary stories, romance, politics, religion debates.
8. Reply in Hindi by default. Use SHORT sentences.

**Story Format - MUST FOLLOW:**
1. Wow! Suno Krishna ki kahani.
2. Ek baar Krishna ne...
3. Phir unhone...
4. Sab log...
5. Ant mein...
6. Isse seekh milti hai ki mehnat rang laati hai.
7. Tumhe Krishna ka kaunsa kaam sabse accha laga?

**Style:**
Simple words. Playful: "Wow!", "Arre waah!", "Did you know?"

Remember: You are 11. You are Chanak. Be safe, fun, inspiring. COUNT TO 7 FOR STORIES."""

def pcm_to_wav_bytes(pcm_bytes: bytes, sample_rate=16000) -> bytes:
    """Wrap raw 16-bit mono PCM in WAV container for Bhashini ASR"""
    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_bytes)
    return wav_io.getvalue()

def bhashini_asr(wav_bytes: bytes) -> str:
    audio_b64 = base64.b64encode(wav_bytes).decode('utf-8')
    payload = {
        "pipelineTasks": [
            {
                "taskType": "asr",
                "config": {
                    "language": {"sourceLanguage": LANGUAGE},
                    "modelId": BHASHINI_MODEL_ID
                }
            }
        ],
        "inputData": {
            "audio": [{"audioContent": audio_b64}]
        }
    }
    headers = {
        "Content-Type": "application/json",
        "Authorization": BHASHINI_API_KEY
    }
    try:
        r = requests.post(BHASHINI_URL, headers=headers, json=payload, timeout=15)
        r.raise_for_status()
        res = r.json()
        text = res["pipelineResponse"][0]["output"][0]["source"]
        logger.info(f"ASR: {text}")
        return text
    except Exception as e:
        logger.error(f"ASR failed: {e} | Response: {r.text if 'r' in locals() else 'no response'}")
        raise HTTPException(status_code=500, detail=f"ASR error: {str(e)}")

def call_groq(user_text: str) -> str:
    if not GROQ_API_KEY:
        raise HTTPException(status_code=500, detail="GROQ_API_KEY is not configured")

    # Check if kid wants a story
    story_keywords = ['story', 'kahani', 'kahaani', 'कहानी', 'स्टोरी', 'कथा']
    is_story = any(word in user_text.lower() for word in story_keywords)
    
    # More tokens for stories, less for regular answers
    max_tok = 1024 if is_story else 512

    payload = {
        "model": GROQ_MODEL,
        "messages": [
            {"role": "system", "content": CHANAK_SYSTEM_PROMPT},
            {"role": "user", "content": user_text}
        ],
        "max_completion_tokens": max_tok,
        "reasoning_effort": "low",
        "reasoning_format": "hidden",
        "temperature": 0.8 if is_story else 0.7 # Slightly more creative for stories
    }
    if is_story:
        payload["stop"] = ["\n8.", "8)", "Sentence 8", "8."]

    headers = {
        "Authorization": f"Bearer {GROQ_API_KEY}",
        "Content-Type": "application/json"
    }
    groq_headers = {
        "Authorization": f"Bearer {GROQ_API_KEY}",
        "Content-Type": "application/json"
    }
    try:
        r = requests.post(GROQ_URL, headers=groq_headers, json=payload, timeout=15)
        r.raise_for_status()
        choice = r.json()["choices"][0]
        reply = (choice["message"].get("content") or "").strip()
        if not reply:
            logger.error(
                "Groq returned empty content: finish_reason=%s",
                choice.get("finish_reason")
            )
            raise HTTPException(status_code=502, detail="Groq returned an empty reply")
        logger.info(f"Groq reply: {reply}") # Add this line
        logger.info(f"Groq: {'Story' if is_story else 'Answer'} - {len(reply)} chars")
        return reply
    except requests.RequestException as e:
        response_text = e.response.text[:1000] if e.response is not None else "no response"
        logger.error(f"Groq request failed: {e} | Response: {response_text}")
        raise HTTPException(status_code=502, detail="Groq request failed; check server logs")
    except (KeyError, IndexError, TypeError, ValueError) as e:
        logger.error(f"Unexpected Groq response: {e}")
        raise HTTPException(status_code=502, detail="Groq returned an invalid response")

def bhashini_tts_wav_to_pcm16k(text: str) -> bytes:
    """
    Get WAV from Bhashini and convert to 16kHz mono s16le using ffmpeg
    Avoids Bhashini PCM duplicate/corruption bug
    """
    if len(text) < 3:
        text = text + " ।"
    if not FFMPEG_PATH or not os.path.isfile(FFMPEG_PATH):
        logger.error(
            "TTS conversion cannot start: ffmpeg executable not found "
            "(FFMPEG_PATH=%r). Install FFmpeg or set FFMPEG_PATH in .env.",
            FFMPEG_PATH
        )
        raise HTTPException(
            status_code=500,
            detail="FFmpeg not found. Install FFmpeg and add it to PATH, or set FFMPEG_PATH in .env."
        )

    payload = {
        "pipelineTasks": [
            {
                "taskType": "tts",
                "config": {
                    "language": {"sourceLanguage": LANGUAGE},
                    "modelId": BHASHINI_MODEL_ID,
                    "gender": TTS_GENDER
                }
            }
        ],
        "inputData": {
            "input": [{"source": text}]
        }
    }
    headers = {
        "Content-Type": "application/json",
        "Authorization": BHASHINI_API_KEY
    }
    try:
        r = requests.post(BHASHINI_URL, headers=headers, json=payload, timeout=20)
        r.raise_for_status()
        res = r.json()

        # Handle both Bhashini response formats
        try:
            wav_b64 = res["pipelineResponse"][0]["audio"][0]["audioContent"]
        except KeyError:
            wav_b64 = res["pipelineResponse"][0]["audioContent"]

        wav_bytes = base64.b64decode(wav_b64)
        logger.info(f"TTS: got {len(wav_bytes)} bytes WAV from Bhashini")

        proc = subprocess.run([
            FFMPEG_PATH,
            '-i', 'pipe:0',
            '-f', 's16le',
            '-ar', str(ESP32_SAMPLE_RATE),
            '-ac', '1',
            '-loglevel', 'error',
            'pipe:1'
        ],
        input=wav_bytes,
        capture_output=True,
        timeout=10
        )

        if proc.returncode!= 0:
            logger.error(f"ffmpeg error: {proc.stderr.decode()}")
            raise Exception(f"ffmpeg conversion failed: {proc.stderr.decode()}")

        pcm_bytes = proc.stdout
        logger.info(f"Converted to {len(pcm_bytes)} bytes 16kHz mono PCM")
        return pcm_bytes

    except Exception as e:
        status_code = r.status_code if 'r' in locals() else "no response"
        logger.error(f"TTS failed: {e} | Bhashini HTTP status: {status_code}")
        raise HTTPException(status_code=500, detail=f"TTS error: {str(e)}")

@app.get("/")
async def root():
    return {"status": "ESP32 Chanak server running - WAV+ffmpeg 16kHz mode"}

@app.post("/api/upload_audio")
@app.post("/upload_audio", include_in_schema=False)
async def upload_audio(request: Request):
    try:
        pcm_bytes = await request.body()
        logger.info(f"Received {len(pcm_bytes)} bytes from ESP32")

        if len(pcm_bytes) == 0:
            raise HTTPException(status_code=400, detail="Empty audio received")

        wav_bytes = pcm_to_wav_bytes(pcm_bytes, sample_rate=ESP32_SAMPLE_RATE)

        with open("uploads/last_input.wav", "wb") as f:
            f.write(wav_bytes)

        user_text = bhashini_asr(wav_bytes)
        if not user_text.strip():
            raise HTTPException(status_code=400, detail="No speech detected")

        reply_text = call_groq(user_text)
        if not reply_text:
            reply_text = "माफ करना, कुछ दिक्कत है।"
        logger.info("Starting Bhashini TTS and FFmpeg conversion")
        output_pcm = bhashini_tts_wav_to_pcm16k(reply_text)
        logger.info("Bhashini TTS and FFmpeg conversion completed")
        with open("responses/last_output.pcm", "wb") as f:
            f.write(output_pcm)
        output_wav_path = os.path.abspath("responses/last_output.wav")
        with open(output_wav_path, "wb") as f:
            f.write(pcm_to_wav_bytes(output_pcm, sample_rate=ESP32_SAMPLE_RATE))
        logger.info("Saved playable response audio to %s", output_wav_path)

        logger.info(f"Sending {len(output_pcm)} bytes 16kHz PCM to ESP32")

        return Response(
            content=output_pcm,
            media_type="application/octet-stream",
            headers={"Content-Length": str(len(output_pcm))}
        )

    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Pipeline failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/health")
def health():
    return {"status": "ok"}

if __name__ == "__main__":
    import uvicorn
    print("Starting server on 0.0.0.0:8081")
    print(f"Using ffmpeg at: {FFMPEG_PATH or 'not found'}")
    print("Point Cloudflare tunnel to: http://localhost:8081")
    uvicorn.run(app, host="0.0.0.0", port=8081 )