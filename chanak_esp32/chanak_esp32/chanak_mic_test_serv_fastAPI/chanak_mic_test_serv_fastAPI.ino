#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <driver/i2s.h>

const char* ssid = "Bhargava";
const char* password = "c0ldl0nd0n";
const char* serverURL = "https://karen-audience-postal-technical.trycloudflare.com/upload_audio";

// I2S Microphone pins - INMP441
#define I2S_SCK 4
#define I2S_WS 5
#define I2S_SD 6
#define I2S_PORT_IN I2S_NUM_0

// I2S Speaker pins - MAX98357A
#define I2S_BCK_OUT 7 // BCLK
#define I2S_WS_OUT 8 // LRC
#define I2S_DATA_OUT 9 // DIN
#define I2S_PORT_OUT I2S_NUM_1

#define BUTTON_PIN 10

#define SAMPLE_RATE 16000
#define RECORD_TIME 3 // seconds
#define BUFFER_SIZE (SAMPLE_RATE * RECORD_TIME * 2) // 16-bit mono

// Match BHASHINI_SAMPLING_RATE from.env
#define TTS_SAMPLE_RATE 16000

uint8_t* audioBuffer;

void setupI2S() {
  // I2S for Microphone
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

  // I2S for Speaker
  i2s_config_t i2s_out_config = {
.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
.sample_rate = TTS_SAMPLE_RATE,
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

void recordAudio() {
  Serial.println("Recording...");
  size_t bytesRead = 0;
  size_t totalBytesRead = 0;
  while (totalBytesRead < BUFFER_SIZE) {
    esp_err_t result = i2s_read(I2S_PORT_IN, audioBuffer + totalBytesRead, BUFFER_SIZE - totalBytesRead, &bytesRead, 100);
    if (result == ESP_OK && bytesRead > 0) {
      totalBytesRead += bytesRead;
    }
  }
  Serial.printf("Recorded %d bytes\n", totalBytesRead);
}

void uploadAndPlay() {
  if(WiFi.status()!= WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  Serial.println("Uploading...");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(*client, serverURL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("Connection", "keep-alive");

  int httpCode = http.POST(audioBuffer, BUFFER_SIZE);

  if (httpCode!= HTTP_CODE_OK) {
    Serial.printf("HTTP POST failed: %d %s\n", httpCode, http.errorToString(httpCode).c_str());
    http.end();
    delete client;
    return;
  }

  Serial.println("Receiving audio...");
  WiFiClient * stream = http.getStreamPtr();

  // Skip 44-byte WAV header from Bhashini
  uint8_t wavHeader[44];
  int headerRead = 0;
  unsigned long headerTimeout = millis();
  while (headerRead < 44 && millis() - headerTimeout < 5000) {
    if (stream->available()) {
      int r = stream->readBytes(wavHeader + headerRead, 44 - headerRead);
      if (r > 0) headerRead += r;
    } else if (!stream->connected()) {
      Serial.println("Stream closed before WAV header");
      http.end();
      delete client;
      return;
    }
    delay(1);
  }

  if (headerRead < 44) {
    Serial.println("Failed to read WAV header");
    http.end();
    delete client;
    return;
  }



uint8_t playBuf;
  ;size_t bytesReceived = 0;
  i2s_zero_dma_buffer(I2S_PORT_OUT);

  Serial.println("Playing...");
  unsigned long dataTimeout = millis();
  while (stream->connected() || stream->available()) {
    if (stream->available()) {
      size_t len = stream->readBytes(playBuf, sizeof(playBuf));
      if (len > 0) {
        size_t bytesWritten;
        i2s_write(I2S_PORT_OUT, playBuf, len, &bytesWritten, portMAX_DELAY);
        bytesReceived += len;
        dataTimeout = millis();
      }
    } else {
      if (millis() - dataTimeout > 5000) {
        Serial.println("Timeout waiting for data");
        break;
      }
      delay(1);
    }
  }

  http.end();
  delete client;
  Serial.printf("Played %d bytes. Done. Hold GPIO10 to talk again\n", bytesReceived);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  audioBuffer = (uint8_t*) malloc(BUFFER_SIZE);
  if (!audioBuffer) {
    Serial.println("Failed to allocate audio buffer");
    while(1);
  }

  setupI2S();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status()!= WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());
  Serial.println("Hold GPIO10 to talk to Chanak");
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      recordAudio();
      uploadAndPlay();
      delay(1000);
    }
  }
}