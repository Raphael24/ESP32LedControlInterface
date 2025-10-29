#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// WiFi Configuration
const char* ssid = "Obergeschoss_ROMANN";
const char* password = "Sommer2024";

// Server Configuration
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// LED Configuration
#define MAX_LEDS_PER_STRIP 50
#define NUM_STRIPS 3

//Set up Statemachine
enum Mode { MODE_OFF, MODE_COLOR, MODE_RAINBOW, MODE_FLASH, MODE_RAINBOW2 };
volatile Mode currentMode = MODE_RAINBOW2;
static unsigned long lastRainbowUpdate = 0;
static unsigned long lastFlashUpdate[NUM_STRIPS] = {0, 0, 0};
static unsigned long lastRainbow2Update[NUM_STRIPS] = {0, 0, 0};
static bool flashState[NUM_STRIPS] = {false, false, false};

// LED Strip Definitions
struct LEDStrip {
  Adafruit_NeoPixel* strip;
  int pin;
  int numLeds;
  String type;
  String name;
  bool active;
  bool masterOn;
  int brightness;
  uint32_t color;
  bool rainbowActive;
  bool rainbow2Active;
  int rainbowSpeed;
  int rainbow2Speed;
  int rainbowHue;
  bool flashActive;
  uint32_t flashColor;
  int flashFrequency;
  unsigned long lastUpdate;
};

LEDStrip strips[NUM_STRIPS];

// LED Pins (adjust as needed)
int ledPins[NUM_STRIPS] = {2, 4, 5};

// Effect Timers (removed - now using static variables in functions)

// JSON Buffer
StaticJsonDocument<1024> jsonBuffer;

void setup() {
  Serial.begin(115200);
  
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  Serial.println("LittleFS Inhalt:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while(file){
    Serial.println(file.name());
    file = root.openNextFile();
  }
  
  // Initialize LED strips
  initializeLEDStrips();
  
  // Load settings from LittleFS
  loadSettingsFromLittleFS();
  
  // Connect to WiFi
  connectToWiFi();
  
  // Setup Web Server
  setupWebServer();
  
  // Setup WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  Serial.println("ESP32 LED Controller Ready!");
  Serial.print("Web Server: http://");
  Serial.println(WiFi.localIP());
  Serial.print("WebSocket: ws://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/ws");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  switch(currentMode){
    case MODE_OFF:
    Serial.println("Off Mode");
      break;
    case MODE_COLOR:
      // Update LED strips
      Serial.println("Color Mode");
      updateLEDStrips();
      break;
    case MODE_RAINBOW:
      // Update LED effects
      Serial.println("Rainbow Mode");
      updateRainbowEffect();
      break;
    case MODE_RAINBOW2:
      // Update LED effects
      Serial.println("Rainbow2 Mode");
      updateRainbow2Effect();
      break;
    case MODE_FLASH:
      Serial.println("Flash Mode");
      updateFlashEffect();
      break;
    default:
      break;
  }
  
  // Small delay to prevent overwhelming the system
  delay(1);
}

void initializeLEDStrips() {
  for (int i = 0; i < NUM_STRIPS; i++) {
    strips[i].pin = ledPins[i];
    strips[i].numLeds = 300; // Default
    strips[i].type = "WS2812B";
    strips[i].name = "LED Strip " + String(i + 1);
    strips[i].active = true;
    strips[i].masterOn = true;
    strips[i].brightness = 200;
    strips[i].color = 0; // Black
    strips[i].rainbowActive = false;
    strips[i].rainbow2Active = true;
    strips[i].rainbowSpeed = 50;
    strips[i].rainbow2Speed = 5000;
    strips[i].rainbowHue = 0;
    //strips[i].rainbowTimer = 0;
    strips[i].flashActive = false;
    strips[i].flashColor = 0xFF0000; // Red
    strips[i].flashFrequency = 5;
    strips[i].lastUpdate = 0;
    
    // Initialize NeoPixel strip
    strips[i].strip = new Adafruit_NeoPixel(MAX_LEDS_PER_STRIP, strips[i].pin, NEO_GRB + NEO_KHZ800);
    strips[i].strip->begin();
    strips[i].strip->setBrightness(128);
    strips[i].strip->clear();
    strips[i].strip->show();
  }
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setupWebServer() {
  // Serve the main HTML file
  server.on("/", HTTP_GET, []() {
    if (LittleFS.exists("/index.html")) {
      File file = LittleFS.open("/index.html", "r");
      server.sendHeader("Content-Type", "text/html");
      server.streamFile(file, "text/html");
      file.close();
    } else {
      server.send(404, "text/plain", "File not found");
    }
  });
  
  // API endpoint for LED control
  server.on("/api/led", HTTP_POST, handleLEDCommand);
  
  // API endpoint for settings
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handleSaveSettings);
  
  // CORS headers
  server.onNotFound([]() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(404, "application/json", "{\"error\":\"Not found\"}");
  });
  
  server.begin();
}

void handleLEDCommand() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  
  if (server.method() == HTTP_OPTIONS) {
    server.send(200, "application/json", "{}");
    return;
  }
  
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    
    // Parse JSON
    DeserializationError error = deserializeJson(jsonBuffer, body);
    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    
    JsonObject json = jsonBuffer.as<JsonObject>();
    
    // Process LED command
    String response = processLEDCommand(json);
    server.send(200, "application/json", response);
  } else {
    server.send(400, "application/json", "{\"error\":\"No data\"}");
  }
}

String processLEDCommand(JsonObject json) {
  int strip = json["strip"];
  String command = json["command"];
  Serial.print("Command: ");
  Serial.println(command);
  
  if (strip < 1 || strip > NUM_STRIPS) {
    return "{\"error\":\"Invalid strip number\"}";
  }
  
  int stripIndex = strip - 1;
  
  if (command == "master") {
    strips[stripIndex].masterOn = json["value"];
    return "{\"status\":\"OK\",\"master\":" + String(strips[stripIndex].masterOn) + "}";
  }
  else if (command == "color") {
    currentMode = MODE_COLOR;
    String colorStr = json["value"];
    strips[stripIndex].color = parseColor(colorStr);
    return "{\"status\":\"OK\",\"color\":\"" + colorStr + "\"}";
  }
  else if (command == "brightness") {
    strips[stripIndex].brightness = json["value"];
    return "{\"status\":\"OK\",\"brightness\":" + String(strips[stripIndex].brightness) + "}";
  }
  else if (command == "rainbow") {
    currentMode = MODE_RAINBOW;
    JsonObject rainbowData = json["value"];
    strips[stripIndex].rainbowActive = rainbowData["start"];
    strips[stripIndex].rainbowSpeed = rainbowData["speed"];
    return "{\"status\":\"OK\",\"rainbow\":" + String(strips[stripIndex].rainbowActive) + "}";
  }
  else if (command == "rainbow2") {
    currentMode = MODE_RAINBOW2;
    JsonObject rainbowData = json["value"];
    strips[stripIndex].rainbow2Active = rainbowData["start"];
    strips[stripIndex].rainbow2Speed = rainbowData["speed"];
    return "{\"status\":\"OK\",\"rainbow2\":" + String(strips[stripIndex].rainbow2Active) + "}";
  }
  else if (command == "flash") {
    currentMode = MODE_FLASH;
    JsonObject flashData = json["value"];
    strips[stripIndex].flashActive = flashData["start"];
    strips[stripIndex].flashColor = parseColor(flashData["color"]);
    strips[stripIndex].flashFrequency = flashData["frequency"];
    return "{\"status\":\"OK\",\"flash\":" + String(strips[stripIndex].flashActive) + "}";
  }
  else if (command == "off") {
    strips[stripIndex].masterOn = false;
    strips[stripIndex].rainbowActive = false;
    strips[stripIndex].flashActive = false;
    strips[stripIndex].color = 0; // Black
    return "{\"status\":\"OK\",\"off\":true}";
  }
  
  return "{\"error\":\"Unknown command\"}";
}

uint32_t parseColor(String colorStr) {
  //Serial.print("Color: ");
  //Serial.print(colorStr);
  if (colorStr.startsWith("#")) {
    colorStr = colorStr.substring(1);
  }
  
  long color = strtol(colorStr.c_str(), NULL, 16);
  //Serial.print(" ");
  //Serial.println(color);
  return (uint32_t)color;
}


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  Serial.print("Websocket Event");
  Serial.println(type);
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("WebSocket client #%u disconnected\n", num);
      break;
      
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("WebSocket client #%u connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      }
      break;
      
    case WStype_TEXT:
      {
        String message = String((char*)payload);
        Serial.printf("WebSocket received: %s\n", message.c_str());
        
        // Parse JSON
        DeserializationError error = deserializeJson(jsonBuffer, message);
        if (!error) {
          JsonObject json = jsonBuffer.as<JsonObject>();
          String response = processLEDCommand(json);
          webSocket.sendTXT(num, response);
        } else {
          webSocket.sendTXT(num, "{\"error\":\"Invalid JSON\"}");
        }
      }
      break;
      
    default:
      break;
  }
}

void updateRainbowEffect() {
  
  static uint16_t rainbowHue = 0;
  
  // Update rainbow every 50ms for smooth animation
  if (millis() - lastRainbowUpdate > 50) {
    lastRainbowUpdate = millis();
  Serial.println("Rainbow update");
  
  for (int i = 0; i < NUM_STRIPS; i++) {
    if (strips[i].rainbowActive && strips[i].masterOn) {
      // Calculate speed factor (1-100 -> 1-10 steps per update)
      int speedFactor = map(strips[i].rainbowSpeed, 0, 100, 1, 10);
      
      for (int j = 0; j < strips[i].numLeds; j++) {
        // Calculate hue for each pixel with offset
        uint16_t pixelHue = (rainbowHue + (j * 65536L / strips[i].numLeds)) % 65536;
        strips[i].strip->setPixelColor(j, strips[i].strip->gamma32(strips[i].strip->ColorHSV(pixelHue)));
      }
      
      // Increment hue based on speed
      rainbowHue = (rainbowHue + speedFactor) % 65536;
    }
  strips[i].strip->show();
  }
  }
}


void updateRainbow2Effect(){
  for (int j = 0; j < NUM_STRIPS; j++) {
    // Geschwindigkeit: delay zwischen Updates je Strip
    unsigned long delayRainbow2 = strips[j].rainbow2Speed;

    if (millis() - lastRainbow2Update[j] >= delayRainbow2) {
      lastRainbow2Update[j] = millis();

      // Hue für diesen Strip weiterdrehen
      strips[j].rainbowHue += 256;  // Schrittgröße -> 256 = smooth
      if (strips[j].rainbowHue >= (5 * 65536)) {
        strips[j].rainbowHue = 0;
      }

      // Jede LED gleiche Farbe aber Hue animiert
      for (int i = 0; i < strips[j].numLeds; i++) {
        strips[j].strip->setPixelColor(
          i,
          strips[j].strip->gamma32(
            strips[j].strip->ColorHSV(strips[j].rainbowHue)
          )
        );
      }
      strips[j].strip->show();
    }
  }
}


void updateFlashEffect() {

  for (int i = 0; i < NUM_STRIPS; i++) {
    if (strips[i].flashActive && strips[i].masterOn) {
      // Calculate flash interval based on frequency (1-20 Hz)
      unsigned long flashInterval = 1000 / (1 + strips[i].flashFrequency);
      
      if (millis() - lastFlashUpdate[i] >= flashInterval) {
        lastFlashUpdate[i] = millis();
        flashState[i] = !flashState[i];
        Serial.println("Flash update");
        
        if (flashState[i]) {
          for (int j = 0; j < strips[i].numLeds; j++) {
            strips[i].strip->setPixelColor(j, strips[i].flashColor);
          }
        } else {
          for (int j = 0; j < strips[i].numLeds; j++) {
            strips[i].strip->setPixelColor(j, 0); // Black
          }
        }
      }
    }
    strips[i].strip->show();
  }
}

void updateLEDStrips() {
  for (int i = 0; i < NUM_STRIPS; i++) {
    if (!strips[i].masterOn) {
      // Turn off strip
      strips[i].strip->clear();
    } else {
      // Solid color
      //Serial.print("Solid Color");
      //Serial.println(strips[i].color);
      for (int j = 0; j < strips[i].numLeds; j++) {
        strips[i].strip->setPixelColor(j, strips[i].color);
      }
    }
    
    // Apply brightness
    strips[i].strip->setBrightness(strips[i].brightness);
    strips[i].strip->show();
  }
}

// Additional utility functions
void setStripSettings(int stripIndex, int numLeds, int pin, String type, String name) {
  if (stripIndex >= 0 && stripIndex < NUM_STRIPS) {
    Serial.println("Set Strip Settings: ");
    strips[stripIndex].numLeds = numLeds;
    strips[stripIndex].pin = pin;
    strips[stripIndex].type = type;
    strips[stripIndex].name = name;
    strips[stripIndex].active = true;
  }
}

void saveSettingsToLittleFS() {
  // Save current settings to LittleFS
  File file = LittleFS.open("/settings.json", "w");
  if (file) {
    JsonDocument doc;
    for (int i = 0; i < NUM_STRIPS; i++) {
      doc["strips"][i]["active"] = strips[i].active;
      doc["strips"][i]["numLeds"] = strips[i].numLeds;
      doc["strips"][i]["pin"] = strips[i].pin;
      doc["strips"][i]["type"] = strips[i].type;
      doc["strips"][i]["name"] = strips[i].name;
    }
    serializeJson(doc, file);
    file.close();
    Serial.println("Settings successfully saved");
  }
}

void loadSettingsFromLittleFS() {
  // Load settings from LittleFS
  if (LittleFS.exists("/settings.json")) {
    File file = LittleFS.open("/settings.json", "r");
    if (file) {
      JsonDocument doc;
      deserializeJson(doc, file);
      
      for (int i = 0; i < NUM_STRIPS; i++) {
        if (doc["strips"][i]) {
          strips[i].active = doc["strips"][i]["active"];
          strips[i].numLeds = doc["strips"][i]["numLeds"];
          strips[i].pin = doc["strips"][i]["pin"];
          strips[i].type = doc["strips"][i]["type"].as<String>();
          strips[i].name = doc["strips"][i]["name"].as<String>();
          
          Serial.printf("Loaded Strip %d: %s, %d LEDs, Pin %d\n", 
                       i+1, strips[i].name.c_str(), strips[i].numLeds, strips[i].pin);
        }
      }
      file.close();
      Serial.println("Settings successfully loaded");
    }
  } else {
    Serial.println("No settings.json found, using defaults");
  }
}

void handleGetSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  JsonDocument doc;
  for (int i = 0; i < NUM_STRIPS; i++) {
    doc["strips"][i]["active"] = strips[i].active;
    doc["strips"][i]["numLeds"] = strips[i].numLeds;
    doc["strips"][i]["pin"] = strips[i].pin;
    doc["strips"][i]["type"] = strips[i].type;
    doc["strips"][i]["name"] = strips[i].name;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSaveSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    
    // Parse JSON
    DeserializationError error = deserializeJson(jsonBuffer, body);
    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    
    JsonObject json = jsonBuffer.as<JsonObject>();
    
    // Update settings
    for (int i = 0; i < NUM_STRIPS; i++) {
      if (json["strips"][i]) {
        strips[i].active = json["strips"][i]["active"];
        strips[i].numLeds = json["strips"][i]["leds"];
        strips[i].pin = json["strips"][i]["pin"];
        strips[i].type = json["strips"][i]["type"].as<String>();
        strips[i].name = json["strips"][i]["name"].as<String>();
        
        Serial.printf("Updated Strip %d: %s, %d LEDs, Pin %d\n", 
                     i+1, strips[i].name.c_str(), strips[i].numLeds, strips[i].pin);
        Serial.println("Einstellungen wurden geändert");
        // Initialize NeoPixel strip
        strips[i].strip->clear();
        if (strips[i].strip != nullptr) {
            delete strips[i].strip;
        }
        strips[i].strip = new Adafruit_NeoPixel(strips[i].numLeds, strips[i].pin, NEO_GRB + NEO_KHZ800);
        strips[i].strip->begin();
        strips[i].strip->setBrightness(128);
        strips[i].strip->clear();
        strips[i].strip->show();
      } else {
        Serial.println("NoData avaible");
      }
    }
    
    // Save to LittleFS
    saveSettingsToLittleFS();
    
    server.send(200, "application/json", "{\"status\":\"OK\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"No data\"}");
  }
}
