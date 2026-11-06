#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

//const char* serverURL = "https://chanak-whisper-xxx.ondigitalocean.app/api/upload_audio"; // main.py:8080
//const char* fastapiBase = "https://chanak-api-xxx.ondigitalocean.app"; // app.py:8000


const char* serverURL = "https://armed-attempt-syndrome-intranet.trycloudflare.com/api/upload_audio"; // main.py:8080
const char* fastapiBase = "https://positions-friendship-competition-lil.trycloudflare.com"; // app.py:8000

// I2S Microphone INMP441
#define I2S_SCK 4
#define I2S_WS 5
#define I2S_SD 6
#define I2S_PORT_IN I2S_NUM_0

// I2S Speaker MAX98357A
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

#define CHUNK_SIZE 1024
#define SILENCE_THRESHOLD 2000
#define SILENCE_DURATION_MS 1500
#define MIN_RECORD_MS 1500
#define MAX_AUDIO_SIZE 1500000
#define PCM_READ_IDLE_TIMEOUT_MS 60000

#define AP_PASS "12345678"
#define FW_VERSION "chanak_v4.2"

uint8_t* audioBuffer = nullptr;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

String tyid = "";
bool apMode = true;
bool provisioned = false;
bool wifiConnected = false;
unsigned long wifiConnectStart = 0;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 30000;
unsigned long wifiResetTime = 0;
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

void handleSave() {
  Serial.println("\n=== /save HIT ===");
  if (!server.hasArg("ssid")) {
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
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnectStart = millis();
  apMode = false;
  claimRetryCount = 0;
  Serial.println("AP stopped, connecting to STA...");
}

void fetchDeviceToken() {
  if (WiFi.status()!= WL_CONNECTED) return;
  prefs.begin("wifi", true);
  String userToken = prefs.getString("user_token", ""); // user JWT from web app
  prefs.end();
  if (userToken == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);

  String url = String(fastapiBase) + "/api/toy/token/" + tyid;
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + userToken);
  int code = http.POST(""); // POST with empty body
  String resp = http.getString();
  http.end();

  Serial.printf("Token fetch code: %d\n", code);
  if (code == 200) {
    JsonDocument doc;
    deserializeJson(doc, resp);
    String deviceToken = doc["device_token"];
    prefs.begin("wifi", false);
    prefs.putString("token", deviceToken);
    prefs.putBool("provisioned", true);
    prefs.end();
    provisioned = true;
    claimRetryCount = 0;
    Serial.println("Device token saved! Provisioned.");
    digitalWrite(LED_PIN, LOW);
  } else {
    claimRetryCount++;
  }
}

void checkResetFlag() {
  if (WiFi.status()!= WL_CONNECTED) return;
  prefs.begin("wifi", true);
  String token = prefs.getString("token", "");
  prefs.end();
  if (!token.length()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(fastapiBase) + "/api/my_toys");
  http.addHeader("Authorization", "Bearer " + token);
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    JsonDocument doc;
    deserializeJson(doc, resp);
    JsonArray toys = doc["toys"];
    for (JsonObject toy : toys) {
      if (toy["tyid"] == tyid && toy["reset_wifi_flag"] == true) {
        Serial.println("Reset flag from server! Wiping WiFi only");
        wifiReset();
        break;
      }
    }
  }
  http.end();
}

void wifiReset() {
  Serial.println("WiFi Reset - Keeping TYID and token");
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  delay(1000);
  ESP.restart();
}

void factoryReset() {
  Serial.println("Factory reset - wiping NVS only");
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  delay(500);
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

  Serial.printf("BUFFER_SIZE %d bytes\n", BUFFER_SIZE);
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

void uploadAndPlay(size_t audioSize) {
  if (WiFi.status()!= WL_CONNECTED ||!audioBuffer || audioSize == 0) return;
  prefs.begin("wifi", true);
  String token = prefs.getString("token", "");
  prefs.end();
  if (token == "") {
    Serial.println("No device token for /upload_audio");
    return;
  }
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();
  HTTPClient http;
  http.setTimeout(60000);
  http.begin(*client, serverURL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("X-Tyid", tyid);

  int httpCode = http.POST(audioBuffer, audioSize);
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
    String errorBody = http.getString();
    Serial.printf("Upload/LLM HTTP failed: %d\n", httpCode);
    if (errorBody.length() > 0) {
      Serial.println("Server response: " + errorBody);
    }
    http.end(); delete client; return;
  }
  int contentLen = http.getSize();
  if (contentLen <= 0 || contentLen > MAX_AUDIO_SIZE || contentLen % 2 != 0) {
    Serial.printf("Invalid PCM response length: %d bytes\n", contentLen);
    http.end(); delete client; return;
  }
  Serial.printf("LLM response OK: %d bytes of PCM\n", contentLen);
  digitalWrite(LED_PIN, HIGH);
  i2s_set_clk(I2S_PORT_OUT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_PORT_OUT);
  delay(50);
  WiFiClient * stream = http.getStreamPtr();
  uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.println("Failed to allocate PCM playback buffer");
    digitalWrite(LED_PIN, LOW);
    http.end(); delete client; return;
  }
  size_t totalReceived = 0;
  size_t totalWritten = 0;
  unsigned long lastDataAt = millis();
  bool playbackFailed = false;
  while (totalReceived < (size_t)contentLen) {
    int availableBytes = stream->available();
    if (availableBytes <= 0) {
      if (!http.connected()) {
        Serial.printf("PCM connection closed at %u of %d bytes\n", totalReceived, contentLen);
        break;
      }
      if (millis() - lastDataAt >= PCM_READ_IDLE_TIMEOUT_MS) {
        Serial.printf("PCM download timed out at %u of %d bytes\n", totalReceived, contentLen);
        break;
      }
      delay(2);
      continue;
    }

    size_t bytesToRead = (size_t)availableBytes;
    size_t remaining = (size_t)contentLen - totalReceived;
    if (bytesToRead > remaining) bytesToRead = remaining;
    if (bytesToRead > 4096) bytesToRead = 4096;
    int len = stream->readBytes((char*)buf, bytesToRead);
    if (len <= 0) {
      continue;
    }
    lastDataAt = millis();

    size_t offset = 0;
    while (offset < (size_t)len) {
      size_t written = 0;
      esp_err_t err = i2s_write(
        I2S_PORT_OUT,
        buf + offset,
        len - offset,
        &written,
        portMAX_DELAY
      );

      if (err != ESP_OK || written == 0) {
        Serial.printf("I2S write failed: err=%d, wrote=%u\n", err, written);
        playbackFailed = true;
        break;
      }

      offset += written;
      totalWritten += written;
    }

    totalReceived += len;
    if (playbackFailed) break;
  }
  digitalWrite(LED_PIN, LOW);
  Serial.printf("PCM playback finished: received %u/%d, wrote %u bytes\n",
                totalReceived, contentLen, totalWritten);
  heap_caps_free(buf);
  http.end(); delete client;
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
  server.on("/save", HTTP_POST, handleSave);
  server.on("/save", HTTP_GET, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("AP Mode Started");
  Serial.print("SSID: "); Serial.println(apSSID);
  Serial.print("Pass: "); Serial.println(AP_PASS);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Booting chanak_v4.2 Beta...");
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
  String savedPASS = prefs.getString("pass", "");  
  String token = prefs.getString("token", "");
  bool isProvisioned = prefs.getBool("provisioned", false);
  prefs.end();
  if (savedSSID!= "") {
    Serial.println("Found saved WiFi: " + savedSSID);
    Serial.println("Found saved WiFi: " + savedPASS);
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID,savedPASS);
    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("WiFi connected: " + WiFi.localIP().toString());
      if (token!= "" && isProvisioned) {
        provisioned = true;
        Serial.println("Token found. Starting LLM mode.");
      } else {
        Serial.println("WiFi OK but no device token. Will fetch after user claim.");
        provisioned = false;
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
  if (wifiConnected &&!provisioned) {
    fetchDeviceToken();
    delay(5000);
    return;
  }
  if (provisioned) {
    if (digitalRead(WIFI_RESET_PIN) == LOW) {
      if (wifiResetTime == 0) wifiResetTime = millis();
      if (millis() - wifiResetTime > 5000) {
        Serial.println("WiFi Reset button held 5s - Factory Reset");
        factoryReset();
      }
    } else {
      wifiResetTime = 0;
    }
    if (millis() - lastHeartbeat > heartbeatInterval) {
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