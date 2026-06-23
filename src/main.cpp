#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ── Team ID — must match sender ──
#define TEAM_ID  "TEAM42"

// ── Heltec LoRa 32 V2 pins ──
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SS    18
#define LORA_RST   14
#define LORA_DIO0  26

// ── OLED pins ──
#define OLED_SDA   4
#define OLED_SCL   15
#define OLED_RST   16
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64

// ── WiFi credentials ──
const char* WIFI_SSID     = "Pixel_5870";
const char* WIFI_PASSWORD = "abcd12345";

// ── MQTT broker ──
const char* MQTT_BROKER   = "test.mosquitto.org";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT_ID = "AgroSense_Gateway_TEAM42"; // must be unique on broker

// ── Objects ──
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
WiFiClient       wifiClient;
PubSubClient     mqttClient(wifiClient);

int packetCount = 0;

// ─────────────────────────────────────────────
// Convert node ID string to field number
// NODE1 → "1",  NODE2 → "2", etc.
// ─────────────────────────────────────────────
String nodeToField(String nodeId) {
  // Extract trailing number from e.g. "NODE1"
  for (int i = nodeId.length() - 1; i >= 0; i--) {
    if (!isDigit(nodeId[i])) {
      return nodeId.substring(i + 1); // returns "1", "2", ...
    }
  }
  return "0"; // fallback
}

// ─────────────────────────────────────────────
// WiFi connect
// ─────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Failed to connect — continuing offline");
  }
}

// ─────────────────────────────────────────────
// MQTT connect / reconnect
// ─────────────────────────────────────────────
bool connectMQTT() {
  if (mqttClient.connected()) return true;

  Serial.print("[MQTT] Connecting to ");
  Serial.print(MQTT_BROKER);
  Serial.print(" ... ");

  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("Connected!");
    return true;
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  // OLED init
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);  delay(20);
  digitalWrite(OLED_RST, HIGH);
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed"); while (1);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AgroSense V2");
  display.println("Initializing...");
  display.display();

  // LoRa init
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(915E6)) {
    Serial.println("LoRa failed!"); while (1);
  }
  Serial.println("[LoRa] Ready");

  // WiFi + MQTT init
  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  connectMQTT();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Gateway Ready");
  display.println(WiFi.status() == WL_CONNECTED ? "WiFi: OK" : "WiFi: FAIL");
  display.println(mqttClient.connected()        ? "MQTT : OK" : "MQTT : FAIL");
  display.display();
  delay(1500);
}

// ─────────────────────────────────────────────
void loop() {
  // Keep MQTT connection alive
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      connectMQTT();      // attempt reconnect (non-blocking if broker unreachable)
    }
    mqttClient.loop();    // process MQTT keepalive + incoming messages
  }

  // ── LoRa receive ──
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String msg = "";
  while (LoRa.available()) msg += (char)LoRa.read();

  // Filter out packets not from our team
  if (!msg.startsWith(TEAM_ID)) {
    Serial.println("[IGNORED] " + msg);
    return;
  }

  packetCount++;
  int   rssi = LoRa.packetRssi();
  float snr  = LoRa.packetSnr();

  Serial.println("=== Packet #" + String(packetCount) + " ===");
  Serial.println("Raw : " + msg);
  Serial.println("RSSI: " + String(rssi) + " dBm");
  Serial.println("SNR : " + String(snr)  + " dB");

  // ── Parse:  TEAM42|NODE1|counter|moisture|temp ──
  String fields[5];
  int    fieldIdx = 0;
  String token    = "";
  for (int i = 0; i < (int)msg.length() && fieldIdx < 5; i++) {
    if (msg[i] == '|') { fields[fieldIdx++] = token; token = ""; }
    else                  token += msg[i];
  }
  fields[fieldIdx] = token;

  String nodeId   = fields[1];
  String counter  = fields[2];
  String moisture = fields[3];
  String temp     = fields[4];

  Serial.println("Node    : " + nodeId);
  Serial.println("Temp    : " + temp);
  Serial.println("Moisture: " + moisture);
  Serial.println("--------------------------------");

  // ── MQTT Publish ──
  String fieldNum = nodeToField(nodeId);                        // "1"
  String topic    = "agrosense/field_" + fieldNum + "/temperature"; // agrosense/field_1/temperature

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(topic.c_str(), temp.c_str());
    Serial.println("[MQTT] " + String(ok ? "Published" : "FAILED") +
                   " → " + topic + " = " + temp);
  } else {
    Serial.println("[MQTT] Not connected — skipping publish");
  }

  // ── OLED update ──
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(TEAM_ID + String(" #") + String(packetCount));
  display.println("Node: " + nodeId);
  display.println("Temp : " + temp + " C");
  display.println("Moist: " + moisture);
  display.println("RSSI: " + String(rssi) + "dBm");
  display.println(mqttClient.connected() ? "MQTT: OK" : "MQTT: --");
  display.display();
}