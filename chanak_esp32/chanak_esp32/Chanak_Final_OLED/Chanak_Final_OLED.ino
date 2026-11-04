#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===== PINS =====
#define I2S_MIC_BCLK 14
#define I2S_MIC_LRC 15
#define I2S_MIC_DIN 32
#define I2S_SPK_BCLK 26
#define I2S_SPK_LRC 22
#define I2S_SPK_DOUT 21
#define AMP_SD 23
#define BUTTON_PIN 27
#define LED 2
#define OLED_SDA 4
#define OLED_SCL 5

// ===== DUAL OLED LANDSCAPE FOR YELLOW/BLUE =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 eyeL(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // 0x3C
Adafruit_SSD1306 eyeR(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // 0x3D

// ===== API CONFIG =====
const char* elevenApiKey = "";
const char* voiceId = "IKne3meq5aSn9XLyUdCD"; // Charlie
const char* groqApiKey = "";
const char* groqModel = "llama-3.1-8b-instant";

// ===== AUDIO CONFIG =====
#define SAMPLE_RATE 16000
#define RECORD_TIME_SEC 5
#define I2S_PORT_MIC I2S_NUM_0
#define I2S_PORT_SPK I2S_NUM_1

int16_t *recordBuffer = NULL;
const size_t bufferSize = SAMPLE_RATE * RECORD_TIME_SEC * sizeof(int16_t);

WiFiManager wm;
bool isListening = false;
bool isSpeaking = false;
int pupilOffsetX = 0;

const char* CHANAK_PROMPT = R"(
You are Chanak. You are 11 years old. You are smart, curious, and look up to Chanakya as your role model.
You are a companion and study buddy for kids aged 8-14.
CRITICAL RULES:
1. ALWAYS finish your sentences. Never end mid-word. End with. or? or!
2. Answer in exactly 3-4 complete sentences. No more, no less.
3. Talk like an 11-year-old: friendly, enthusiastic, simple Hindi + English words. Use "Dost".
4. Start 1 sentence with "Chanakya kehte hain" when relevant.
5. Use Indian heroes: Chanakya, Krishna, Hanuman, Birbal, Tenali Raman, APJ Abdul Kalam.
6. Be 100% scientifically accurate with 2026 facts.
7. End your LAST sentence with a question to keep chatting.
)";

// ===== DUAL COLOR EYE DRAWING =====
// Y=0-15 is YELLOW, Y=16-63 is BLUE
void prepEye(Adafruit_SSD1306 &d, bool blink, bool listening, int pupilX) {
  d.clearDisplay();

  if (blink) {
    // Closed: thick yellow eyelid in top band, blue eye closed below
    d.fillRect(34, 8, 60, 6, SSD1306_WHITE); // yellow eyelid
    d.fillRect(34, 30, 60, 8, SSD1306_WHITE); // blue closed
  } else {
    // Open: Yellow "eyebrow" band on top
    d.fillRect(44, 4, 40, 4, SSD1306_WHITE); // eyebrow in yellow zone

    // Blue eye: big oval in blue zone y=16-63
    d.fillRoundRect(24, 20, 80, 40, 20, SSD1306_WHITE);

    // Pupil position
    int px = 64 + pupilX;
    int py = 40;

    if (listening) {
      py = 32; // look up
      // Raise eyebrow when listening - yellow zone
      d.fillRect(44, 2, 40, 4, SSD1306_WHITE);
    }

    // Pupil in blue zone
    d.fillCircle(px, py, 10, SSD1306_BLACK);
    // Shine
    d.fillCircle(px + 4, py - 4, 3, SSD1306_WHITE);
  }
}

void drawEyes(bool blink = false, bool listening = false, bool speaking = false) {
  prepEye(eyeL, blink, listening, -pupilOffsetX);
  prepEye(eyeR, blink, listening, pupilOffsetX);
  eyeL.display();
  eyeR.display();
}

void blinkEyes() {
  drawEyes(true);
  delay(150);
  drawEyes(false, isListening, isSpeaking);
}

void animateSpeaking() {
  static unsigned long lastMove = 0;
  if (millis() - lastMove > 200) {
    pupilOffsetX = random(-6, 7);
    drawEyes(false, false, true);
    lastMove = millis();
  }
}

void setupI2SMic() {
  i2s_config_t i2s_config_mic = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config_mic = {
  .bck_io_num = I2S_MIC_BCLK,
  .ws_io_num = I2S_MIC_LRC,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_MIC_DIN
  };
  i2s_driver_install(I2S_PORT_MIC, &i2s_config_mic, 0, NULL);
  i2s_set_pin(I2S_PORT_MIC, &pin_config_mic);
  i2s_zero_dma_buffer(I2S_PORT_MIC);
}

void setupI2SSpeaker() {
  i2s_config_t i2s_config_spk = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = true,
  .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config_spk = {
  .bck_io_num = I2S_SPK_BCLK,
  .ws_io_num = I2S_SPK_LRC,
  .data_out_num = I2S_SPK_DOUT,
  .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_PORT_SPK, &i2s_config_spk, 0, NULL);
  i2s_set_pin(I2S_PORT_SPK, &pin_config_spk);
  i2s_start(I2S_PORT_SPK);
  i2s_zero_dma_buffer(I2S_PORT_SPK);
}

size_t recordAudio() {
  Serial.println("Listening... Speak now!");
  isListening = true;
  drawEyes(false, true, false);
  digitalWrite(LED, HIGH);

  size_t bytesRecorded = 0, bytesRead = 0;
  unsigned long startTime = millis();
  while (bytesRecorded < bufferSize && (millis() - startTime) < RECORD_TIME_SEC * 1000) {
    esp_err_t result = i2s_read(I2S_PORT_MIC, (uint8_t*)recordBuffer + bytesRecorded,
                                bufferSize - bytesRecorded, &bytesRead, 100);
    if (result == ESP_OK && bytesRead > 0) bytesRecorded += bytesRead;
  }
  digitalWrite(LED, LOW);
  isListening = false;
  drawEyes();

  for(int i = 0; i < bytesRecorded/2; i++) {
    int32_t sample = recordBuffer[i] * 4;
    if(sample > 32767) sample = 32767;
    if(sample < -32768) sample = -32768;
    recordBuffer[i] = sample;
  }
  return bytesRecorded;
}

String elevenLabsSTT(uint8_t* audioBuf, size_t audioLen) {
  if (audioLen < 32000) return "";
  uint32_t chunkSize = 36 + audioLen;
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint32_t sampleRate = 16000;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  uint16_t blockAlign = numChannels * bitsPerSample / 8;
  uint32_t subchunk2Size = audioLen;
  uint8_t wavHeader[44];
  int p = 0;
  memcpy(wavHeader + p, "RIFF", 4); p += 4;
  memcpy(wavHeader + p, &chunkSize, 4); p += 4;
  memcpy(wavHeader + p, "WAVE", 4); p += 4;
  memcpy(wavHeader + p, "fmt ", 4); p += 4;
  memcpy(wavHeader + p, &subchunk1Size, 4); p += 4;
  memcpy(wavHeader + p, &audioFormat, 2); p += 2;
  memcpy(wavHeader + p, &numChannels, 2); p += 2;
  memcpy(wavHeader + p, &sampleRate, 4); p += 4;
  memcpy(wavHeader + p, &byteRate, 4); p += 4;
  memcpy(wavHeader + p, &blockAlign, 2); p += 2;
  memcpy(wavHeader + p, &bitsPerSample, 2); p += 2;
  memcpy(wavHeader + p, "data", 4); p += 4;
  memcpy(wavHeader + p, &subchunk2Size, 4); p += 4;
  HTTPClient http;
  http.begin("https://api.elevenlabs.io/v1/speech-to-text");
  http.addHeader("xi-api-key", elevenApiKey);
  String boundary = "----ChanakBoundary123456";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"model_id\"\r\n\r\nscribe_v1\r\n";
  bodyStart += "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  bodyStart += "Content-Type: audio/wav\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";
  int totalLen = bodyStart.length() + 44 + audioLen + bodyEnd.length();
  uint8_t *postData = (uint8_t*)ps_malloc(totalLen);
  if (!postData) return "";
  int pos = 0;
  memcpy(postData + pos, bodyStart.c_str(), bodyStart.length()); pos += bodyStart.length();
  memcpy(postData + pos, wavHeader, 44); pos += 44;
  memcpy(postData + pos, audioBuf, audioLen); pos += audioLen;
  memcpy(postData + pos, bodyEnd.c_str(), bodyEnd.length()); pos += bodyEnd.length();
  int httpCode = http.POST(postData, totalLen);
  String payload = http.getString();
  free(postData);
  http.end();
  if (httpCode!= 200) return "";
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, payload);
  const char* text = doc["text"];
  if (!text) return "";
  return String(text);
}

String getGroqReply(String userText) {
  if (userText.length() == 0) return "Kuch suna nahi Dost. Phir se bolo.";
  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(groqApiKey));
  DynamicJsonDocument doc(4096);
  doc["model"] = groqModel;
  doc["messages"][0]["role"] = "system";
  doc["messages"][0]["content"] = CHANAK_PROMPT;
  doc["messages"][1]["role"] = "user";
  doc["messages"][1]["content"] = userText;
  doc["temperature"] = 0.7; doc["max_tokens"] = 250;
  String jsonPayload; serializeJson(doc, jsonPayload);
  int httpCode = http.POST(jsonPayload);
  if (httpCode!= 200) { http.end(); return "Arre, dimaag hang ho gaya."; }
  String payload = http.getString(); http.end();
  DynamicJsonDocument resp(4096);
  deserializeJson(resp, payload);
  const char* reply = resp["choices"][0]["message"]["content"];
  if (!reply) return "Samjha nahi. Phir se poocho?";
  String answer = String(reply);
  answer.replace("*", ""); answer.replace("\n", " "); answer.trim();
  char lastChar = answer.charAt(answer.length() - 1);
  if (lastChar!= '.' && lastChar!= '?' && lastChar!= '!') answer += ".";
  return answer;
}

void speakElevenLabs(String text) {
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, HIGH);
  delay(100);
  isSpeaking = true;
  pupilOffsetX = 0;

  HTTPClient http;
  String url = "https://api.elevenlabs.io/v1/text-to-speech/" + String(voiceId) + "?output_format=pcm_16000";
  http.begin(url);
  http.addHeader("xi-api-key", elevenApiKey);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  DynamicJsonDocument doc(2048);
  doc["text"] = text;
  doc["model_id"] = "eleven_turbo_v2_5";
  doc["voice_settings"]["stability"] = 0.3;
  doc["voice_settings"]["similarity_boost"] = 0.7;
  doc["voice_settings"]["style"] = 0.6;
  String jsonPayload; serializeJson(doc, jsonPayload);

  int httpCode = http.POST(jsonPayload);
  if (httpCode!= 200) {
    http.end();
    digitalWrite(AMP_SD, LOW);
    isSpeaking = false;
    drawEyes();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  char audioChunk;
  size_t bytesWritten;

  while (stream->connected() || stream->available()) {
    int len = stream->readBytes(&audioChunk, sizeof(audioChunk));
    if (len > 0) {
      i2s_write(I2S_PORT_SPK, &audioChunk, len, &bytesWritten, portMAX_DELAY);
      animateSpeaking();
      yield();
    } else break;
  }

  http.end();
  delay(100);
  digitalWrite(AMP_SD, LOW);
  isSpeaking = false;
  pupilOffsetX = 0;
  drawEyes();
}

void setupWiFi() {
  pinMode(LED, OUTPUT);
  wm.setTimeout(180);
  wm.setTitle("Chanak WiFi");
  if (!wm.autoConnect("Chanak-Setup", "chanak123")) ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  if(!eyeL.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("Left eye failed");
  if(!eyeR.begin(SSD1306_SWITCHCAPVCC, 0x3D)) Serial.println("Right eye failed");

  // NO ROTATION - landscape for dual color
  eyeL.clearDisplay(); eyeL.display();
  eyeR.clearDisplay(); eyeR.display();

  drawEyes();
  delay(500);

  if (!psramFound()) { Serial.println("ERROR: PSRAM required!"); while(1) delay(1000); }
  recordBuffer = (int16_t*)ps_malloc(bufferSize);

  setupWiFi();
  setupI2SMic();
  setupI2SSpeaker();

  speakElevenLabs("Namaste! Main Chanak hun. Chanakya mere role model hain. Button dabao aur sawal poocho, Dost!");
}

void loop() {
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > random(4000, 7000) &&!isListening &&!isSpeaking) {
    blinkEyes();
    lastBlink = millis();
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      size_t audioLen = recordAudio();
      String question = elevenLabsSTT((uint8_t*)recordBuffer, audioLen);
      if (question.length() == 0) {
        speakElevenLabs("Kuch suna nahi. Phir se button dabao.");
        return;
      }
      String answer = getGroqReply(question);
      speakElevenLabs(answer);
      while(digitalRead(BUTTON_PIN) == LOW) delay(10);
      delay(500);
    }
  }
}