#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

// chanak_ts_v2: Server 1: STT/TTS - UNCHANGED
const char* serverURL = "https://polyester-roulette-sporting-richards.trycloudflare.com/upload_audio";

// chanak_ts_v2: Server 2: FastAPI Auth/CRUD - UPDATE TO YOUR V2 TUNNEL
const char* fastapiBase = "https://harbor-kijiji-virgin-candidate.trycloudflare.com"; // NO trailing /

// I2S Microphone pins - INMP441
#define I2S_SCK 4
#define I2S_WS 5
#define I2S_SD 6
#define I2S_PORT_IN I2S_NUM_0

// I2S Speaker pins - MAX98357A
#define I2S_BCK_OUT 7
#define I2S_WS_OUT 8
#define I2S_DATA_OUT 9
#define I2S_PORT_OUT I2S_NUM_1

#define BUTTON_PIN 10
#define LED_PIN 13
#define WIFI_RESET_PIN 15

#define SAMPLE_RATE 16000
#define MAX_RECORD_TIME 8
#define BUFFER_SIZE (SAMPLE_RATE * MAX_RECORD_TIME * 2)

// VAD Settings
#define CHUNK_SIZE 1024
#define SILENCE_THRESHOLD 2000
#define SILENCE_DURATION_MS 1500
#define MIN_RECORD_MS 1500
#define MAX_AUDIO_SIZE 1500000

#define AP_PASS "12345678"

uint8_t* audioBuffer = nullptr;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

// chanak_ts_v2: State machine - removed polling vars
String tyid = "";
bool apMode = true;
bool provisioned = false;
bool wifiConnected = false;
unsigned long lastPrompt = 0;
unsigned long wifiConnectStart = 0;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 30000; // chanak_ts_v2: 30s. Use 60000 for scale
unsigned long wifiResetTime = 0;
// chanak_ts_v2: Added for /hello retry
unsigned long lastClaimAttempt = 0;
int claimRetryCount = 0;

void setupI2S() {
  i2s_config_t i2s_in_config = {
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

  i2s_pin_config_t pin_config_in = {
.bck_io_num = I2S_SCK,
.ws_io_num = I2S_WS,
.data_out_num = I2S_PIN_NO_CHANGE,
.data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT_IN, &i2s_in_config, 0, NULL);
  i2s_set_pin(I2S_PORT_IN, &pin_config_in);

  i2s_config_t i2s_out_config = {
.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
.sample_rate = SAMPLE_RATE,
.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
.communication_format = I2S_COMM_FORMAT_STAND_I2S,
.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
.dma_buf_count = 8,
.dma_buf_len = 1024,
.use_apll = true,
.tx_desc_auto_clear = true,
.fixed_mclk = 0
  };

  i2s_pin_config_t pin_config_out = {
.bck_io_num = I2S_BCK_OUT,
.ws_io_num = I2S_WS_OUT,
.data_out_num = I2S_DATA_OUT,
.data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_PORT_OUT, &i2s_out_config, 0, NULL);
  i2s_set_pin(I2S_PORT_OUT, &pin_config_out);
  i2s_zero_dma_buffer(I2S_PORT_OUT);
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta charset="utf-8">
  <title>Connect Toy</title>
  <style>
    *{box-sizing:border-box}
    body{font-family:-apple-system,sans-serif;padding:20px;background:#f0f0f0;margin:0}
.card{background:#fff;padding:24px;border-radius:16px;box-shadow:0 4px 20px rgba(0,0,0,0.1)}
    h2{color:#7C3AED;margin:0 0 16px 0;font-size:22px}
    input{width:100%;padding:14px;margin:8px 0;border:2px solid #e5e7eb;border-radius:12px;font-size:16px}
    input:focus{outline:none;border-color:#7C3AED}
    button{width:100%;padding:14px;background:#7C3AED;color:#fff;border:none;border-radius:12px;font-size:16px;font-weight:600;margin:12px 0}
  </style>
</head>
<body>
  <div class="card">
    <h2>🏠 Connect Your Toy</h2>
    <form method="POST" action="/save">
      <input name="ssid" placeholder="Home WiFi Name" required autocomplete="off">
      <input name="pass" type="password" placeholder="Home WiFi Password" autocomplete="off">
      <button type="submit">🚀 Connect</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handlePing() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "pong");
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// chanak_ts_v2: No reboot after save. Direct connect + doClaim
void handleSave() {
  Serial.println("\n=== /save HIT ===");
  if (!server.hasArg("ssid")) {
    Serial.println("Missing SSID");
    server.send(400, "text/plain", "Missing SSID");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  Serial.println("Got SSID: " + ssid);

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  server.send(200, "text/html", "<h2>Connecting...</h2><p>Return to app. Toy will auto-connect.</p>");

  // chanak_ts_v2: Don't reboot. Connect directly
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnectStart = millis();
  apMode = false;
  claimRetryCount = 0; // Reset retry counter
  Serial.println("AP stopped, connecting to STA...");
}

// chanak_ts_v2: New function - replaces pollForToken()
void doClaim() {
  if (WiFi.status()!= WL_CONNECTED) return;

  // chanak_ts_v2: Exponential backoff: 2s, 4s, 8s, 16s, 30s max
  unsigned long backoff = min((1UL << claimRetryCount) * 2000, 30000UL);
  if (millis() - lastClaimAttempt < backoff) return;
  lastClaimAttempt = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(fastapiBase) + "/api/hello");
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["tyid"] = tyid;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  Serial.printf("doClaim attempt %d: %d\n", claimRetryCount, code);

  if (code == 200) {
    String resp = http.getString();
    JsonDocument resDoc;
    deserializeJson(resDoc, resp);
    String token = resDoc["token"];

    prefs.begin("wifi", false);
    prefs.putString("token", token);
    prefs.putBool("provisioned", true);
    prefs.end();

    provisioned = true;
    claimRetryCount = 0;
    Serial.println("Claim success! Provisioned.");
    digitalWrite(LED_PIN, LOW);
  }
  else if (code == 404) {
    Serial.println("No pending claim. User hasn't scanned QR yet.");
    claimRetryCount++;
  }
  else if (code == 409) {
    Serial.println("Claim expired. User must scan QR again.");
    claimRetryCount++;
  }
  else {
    Serial.printf("Claim error %d. Retrying...\n", code);
    claimRetryCount++;
  }
  http.end();
}

// chanak_ts_v2: Check if token still valid. Handle 401 = deleted
bool validateToken() {
  prefs.begin("wifi", true);
  String token = prefs.getString("token", "");
  prefs.end();
  if (token == "") return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(fastapiBase) + "/api/device/validate");
  http.addHeader("Authorization", "Bearer " + token);
  int code = http.GET();
  http.end();
  Serial.printf("Token validation: %d\n", code);

  // chanak_ts_v2: 401 = toy deleted from app
  if (code == 401) {
    Serial.println("Token revoked. Wiping token, re-claiming.");
    prefs.begin("wifi", false);
    prefs.remove("token");
    prefs.putBool("provisioned", false);
    prefs.end();
    provisioned = false;
    claimRetryCount = 0;
    return false;
  }
  return code == 200;
}

void factoryReset() {
  Serial.println("Factory reset - wiping NVS");
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  delay(500);
  ESP.restart();
}

void startAPMode() {
  apMode = true;
  provisioned = false;
  wifiConnected = false;
  claimRetryCount = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPsetHostname("chanak-setup");
  String apSSID = "Chanak-" + tyid;
  WiFi.softAP(apSSID.c_str(), AP_PASS);

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/save", HTTP_GET, handleSave);
  server.on("/save", HTTP_OPTIONS, handleOptions);
  server.on("/generate_204", handleRoot);
  server.on("/hotspot-detect.html", handleRoot);
  server.on("/ncsi.txt", handleRoot);
  server.on("/connecttest.txt", handleRoot);
  server.onNotFound(handleRoot);
  server.begin();

  Serial.println("AP Mode Started");
  Serial.print("SSID: "); Serial.println(apSSID);
  Serial.print("Pass: "); Serial.println(AP_PASS);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());
}

// chanak_ts_v2: Option 1 - Heartbeat with auth
bool sendHeartbeat() {
  prefs.begin("wifi", true);
  String token = prefs.getString("token", "");
  prefs.end();

  if (!token.length()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(fastapiBase) + "/api/heartbeat");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);

  String payload = "{\"tyid\":\"" + String(tyid) + "\"}";
  int code = http.POST(payload);
  http.end();

  if (code == 401) {
    Serial.println("Heartbeat: 401 - Token revoked");
    prefs.begin("wifi", false);
    prefs.remove("token");
    prefs.putBool("provisioned", false);
    prefs.end();
    provisioned = false;
    claimRetryCount = 0;
    return false;
  }

  Serial.printf("Heartbeat: %d\n", code);
  return code == 200;
}

void checkResetFlag() {
  if (WiFi.status()!= WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(fastapiBase) + "/api/toy/status/" + tyid);
  int code = http.GET();

  if (code == 200) {
    String resp = http.getString();
    JsonDocument doc;
    deserializeJson(doc, resp);
    if (doc["reset_wifi"]) {
      Serial.println("Reset flag from server! Wiping WiFi only");
      wifiReset();
    }
  }
  http.end();
}

void wifiReset() {
  Serial.println("WiFi Reset - Keeping TYID and provision status");
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  delay(1000);
  ESP.restart();
}

size_t recordAudioWithVAD() {
  if (!audioBuffer) return 0;
  digitalWrite(LED_PIN, HIGH);
  Serial.println("Listening...");

  size_t totalBytesRead = 0;
  int silenceChunks = 0;
  int maxSilenceChunks = SILENCE_DURATION_MS / 64;
  unsigned long startTime = millis();

  while (totalBytesRead < BUFFER_SIZE) {
    size_t bytesRead = 0;
    uint8_t chunk[CHUNK_SIZE * 2];
    esp_err_t result = i2s_read(I2S_PORT_IN, chunk, CHUNK_SIZE * 2, &bytesRead, 100);

    if (result == ESP_OK && bytesRead > 0) {
      memcpy(audioBuffer + totalBytesRead, chunk, bytesRead);
      totalBytesRead += bytesRead;

      int16_t *samples = (int16_t*)chunk;
      int32_t sum = 0;
      int sampleCount = bytesRead / 2;
      for (int i = 0; i < sampleCount; i++) sum += abs(samples[i]);
      int32_t avg = sum / sampleCount;

      if (avg < SILENCE_THRESHOLD) {
        silenceChunks++;
        if (silenceChunks >= maxSilenceChunks && (millis() - startTime) >= MIN_RECORD_MS) break;
      } else {
        silenceChunks = 0;
      }
    }
    if (millis() - startTime > MAX_RECORD_TIME * 1000) break;
  }

  digitalWrite(LED_PIN, LOW);
  Serial.printf("Recorded %d bytes\n", totalBytesRead);
  return totalBytesRead;
}

// chanak_ts_v2: Added 401 handling for /upload_audio
void uploadAndPlay(size_t audioSize) {
  if (WiFi.status()!= WL_CONNECTED ||!audioBuffer || audioSize == 0) return;

  prefs.begin("wifi", true);
  String token = prefs.getString("token", "");
  prefs.end();
  if (token == "") {
    Serial.println("No token for /llm");
    return;
  }

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();
  HTTPClient http;
  http.setTimeout(60000);
  http.begin(*client, serverURL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("Authorization", "Bearer " + token); // chanak_ts_v2: Send JWT
  http.addHeader("X-Tyid", tyid);

  int httpCode = http.POST(audioBuffer, audioSize);

  // chanak_ts_v2: 401 = deleted. Trigger re-claim
  if (httpCode == 401) {
    Serial.println("LLM 401: Toy deleted. Re-claiming.");
    prefs.begin("wifi", false);
    prefs.remove("token");
    prefs.putBool("provisioned", false);
    prefs.end();
    provisioned = false;
    claimRetryCount = 0;
    http.end(); delete client; return;
  }

  if (httpCode!= 200) {
    Serial.printf("STT HTTP failed: %d\n", httpCode);
    http.end(); delete client; return;
  }

  int contentLen = http.getSize();
  if (contentLen <= 0 || contentLen > MAX_AUDIO_SIZE) {
    http.end(); delete client; return;
  }

  digitalWrite(LED_PIN, HIGH);
  i2s_set_clk(I2S_PORT_OUT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_PORT_OUT);
  delay(50);

  WiFiClient * stream = http.getStreamPtr();
  uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!buf) {
    digitalWrite(LED_PIN, LOW);
    http.end(); delete client; return;
  }

  while (stream->connected() || stream->available()) {
    int len = stream->readBytes((char*)buf, 4096);
    if (len <= 0) break;
    size_t bytesWritten;
    i2s_write(I2S_PORT_OUT, buf, len, &bytesWritten, portMAX_DELAY);
  }

  digitalWrite(LED_PIN, LOW);
  heap_caps_free(buf);
  http.end(); delete client;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Booting chanak_ts_v2...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);

  audioBuffer = (uint8_t*)heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!audioBuffer) {
    Serial.println("Failed to allocate audioBuffer!");
    while(1) delay(1000);
  }
  setupI2S();

  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);

  uint64_t chipid = ESP.getEfuseMac();
  tyid = "TY-" + String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
  tyid.toUpperCase();
  tyid.trim();
  Serial.println("=== DEVICE TYID: " + tyid + " ===");

  prefs.begin("wifi", true);
  String savedSSID = prefs.getString("ssid", "");
  bool hasToken = prefs.getString("token", "")!= "";
  prefs.end();

  if (savedSSID!= "") {
    Serial.println("Found saved WiFi: " + savedSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("WiFi connected: " + WiFi.localIP().toString());

      if (hasToken) {
        if (validateToken()) {
          provisioned = true;
          Serial.println("Token valid. Starting LLM mode.");
        } else {
          Serial.println("Token invalid. Will re-claim via /hello.");
          provisioned = false;
          claimRetryCount = 0;
        }
      } else {
        Serial.println("WiFi OK but no token. Will claim via /hello.");
        provisioned = false;
        claimRetryCount = 0;
      }
      apMode = false;
      return;
    } else {
      Serial.println("WiFi connect failed");
    }
  }

  Serial.println("Starting AP mode");
  startAPMode();
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  }

  if (!wifiConnected && wifiConnectStart > 0 && millis() - wifiConnectStart > 30000) {
    Serial.println("WiFi failed. Restarting AP");
    startAPMode();
    return;
  }

  // chanak_ts_v2: No polling. Just call doClaim() with backoff
  if (wifiConnected &&!provisioned) {
    doClaim();

    if (millis() - lastPrompt > 30000) {
      Serial.println("Playing: Please add to account");
      lastPrompt = millis();
    }
    return;
  }

  if (provisioned) {
    if (digitalRead(WIFI_RESET_PIN) == LOW) {
      if (wifiResetTime == 0) wifiResetTime = millis();
      if (millis() - wifiResetTime > 5000) {
        Serial.println("WiFi Reset button held 5s");
        wifiReset();
      }
    } else {
      wifiResetTime = 0;
    }

    if (millis() - lastHeartbeat > heartbeatInterval) {
      sendHeartbeat();
      checkResetFlag();
      lastHeartbeat = millis();
    }

    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);
      if (digitalRead(BUTTON_PIN) == LOW) {
        size_t recordedBytes = recordAudioWithVAD();
        if (recordedBytes > 0) uploadAndPlay(recordedBytes);
        delay(1000);
      }
    }
  }
}