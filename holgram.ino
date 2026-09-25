#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Preferences.h>

// --- Pin Definitions (XIAO ESP32S3 Defaults) ---
#define TFT_CS     2
#define TFT_RST    3
#define TFT_DC     4
#define TFT_MOSI   9
#define TFT_SCLK   7

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

Preferences preferences;
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

enum DisplayMode { MODE_CLOCK, MODE_LYRICS, MODE_3D_OBJECT, MODE_CUSTOM_TEXT, MODE_VU_METER };
DisplayMode currentMode = MODE_CLOCK;

enum ShapeType { SHAPE_HYPERCUBE, SHAPE_GYRO, SHAPE_COMPLEX_GEM, SHAPE_ICOSAHEDRON, SHAPE_TORUS };
ShapeType currentShape = SHAPE_HYPERCUBE;

// --- Color Configuration (RGB565) ---
uint16_t primaryTextColor = ST7735_CYAN;
uint16_t accentColor       = ST7735_YELLOW;
uint16_t backgroundColor   = ST7735_BLACK;

// --- Banner & Lyrics Variables ---
String currentLyric = "Hologram System Ready!";
String customText   = "Cyber Hologram OS";

int lyricScrollY = 128;
unsigned long lastScrollTime = 0;
int lastSec = -1;

// --- Audio VU Meter Variables ---
int currentAudioLevel = 0; // 0 to 100

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800; // IST Offset (+5:30)
const int   daylightOffset_sec = 0;

float angleX = 0, angleY = 0, angleZ = 0;

// Helper: Convert HTML Hex (#RRGGBB) to 16-bit RGB565 format
uint16_t hexTo565(String hex) {
  if (hex.startsWith("#")) hex = hex.substring(1);
  long number = strtol(hex.c_str(), NULL, 16);
  uint8_t r = (number >> 16) & 0xFF;
  uint8_t g = (number >> 8) & 0xFF;
  uint8_t b = number & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void applyHorizontalMirror() {
  tft.sendCommand(ST77XX_MADCTL, (const uint8_t[]){0x20}, 1);
}

// --- WiFi Setup Portal HTML Page ---
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiFi Setup - Hologram OS</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #080810; color: #00e5ff; text-align: center; padding: 20px; }
    .card { background: #121222; padding: 20px; border-radius: 10px; max-width: 360px; margin: 0 auto; border: 1px solid #00e5ff33; }
    input[type=text], input[type=password] { width: 90%; padding: 10px; margin: 8px 0; border-radius: 6px; border: 1px solid #00e5ff66; background: #000; color: #fff; }
    button { background: #00e5ff; border: none; color: #000; padding: 10px 20px; font-weight: bold; border-radius: 6px; cursor: pointer; width: 95%; margin-top: 10px; }
  </style>
</head>
<body>
  <div class="card">
    <h2>WiFi Configuration</h2>
    <form action="/save" method="POST">
      <input type="text" name="ssid" placeholder="WiFi SSID (Name)" required><br>
      <input type="password" name="pass" placeholder="WiFi Password"><br>
      <button type="submit">Save & Restart</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

// --- Main Control Dashboard HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Hologram Master Control</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #080810; color: #00e5ff; text-align: center; margin: 0; padding: 12px; }
    h2 { text-shadow: 0 0 10px #00e5ff; margin-bottom: 8px; }
    .card { background: #121222; padding: 12px; border-radius: 10px; margin: 10px auto; max-width: 440px; border: 1px solid #00e5ff33; }
    button { background: #00e5ff; border: none; color: #000; padding: 8px 12px; font-weight: bold; border-radius: 6px; cursor: pointer; margin: 3px; }
    button:hover { background: #ffffff; }
    .color-picker-group { display: flex; justify-space-around; align-items: center; margin: 8px 0; }
    .color-picker-item { display: flex; flex-direction: column; align-items: center; font-size: 12px; }
    input[type=color] { border: none; width: 40px; height: 40px; border-radius: 50%; cursor: pointer; background: none; }
    input[type=text] { width: 90%; padding: 8px; border-radius: 6px; border: 1px solid #00e5ff66; background: #000; color: #fff; margin-bottom: 6px; }
    input[type=range] { width: 90%; }
  </style>
</head>
<body>
  <h2>Hologram Master Dashboard</h2>
  
  <div class="card">
    <b>Mode Selection</b><br><br>
    <button onclick="setMode('clock')">Clock HUD</button>
    <button onclick="setMode('3d')">3D Wireframes</button>
    <button onclick="setMode('lyrics')">Lyrics Stream</button>
    <button onclick="setMode('text')">Custom Banner</button>
    <button onclick="setMode('vu')">Audio VU Meter</button>
  </div>

  <div class="card">
    <b>Custom Component Colors</b>
    <div class="color-picker-group">
      <div class="color-picker-item">
        <span>Text/Primary</span>
        <input type="color" id="primaryPicker" value="#00e5ff" onchange="sendColor('primary', this.value)">
      </div>
      <div class="color-picker-item">
        <span>Accents/Borders</span>
        <input type="color" id="accentPicker" value="#ffff00" onchange="sendColor('accent', this.value)">
      </div>
      <div class="color-picker-item">
        <span>Background</span>
        <input type="color" id="bgPicker" value="#000000" onchange="sendColor('bg', this.value)">
      </div>
    </div>
  </div>

  <div class="card">
    <b>3D Shape Selector</b><br><br>
    <button onclick="setShape('hypercube')">Tesseract</button>
    <button onclick="setShape('gyro')">Gyroscope</button>
    <button onclick="setShape('gem')">Octa Gem</button>
    <button onclick="setShape('icosahedron')">Icosahedron</button>
    <button onclick="setShape('torus')">Torus Ring</button>
  </div>

  <div class="card">
    <b>Text & Lyrics Controller</b>
    <input type="text" id="textInput" placeholder="Enter custom message...">
    <button style="width:95%" onclick="sendText()">Update Banner</button>
  </div>

  <div class="card">
    <b>Audio Test Slider (Manual Input)</b><br>
    <input type="range" min="0" max="100" value="0" oninput="sendAudio(this.value)">
  </div>

  <script>
    var ws;
    function initWebSocket() { 
      ws = new WebSocket(`ws://${window.location.hostname}:81/`);
    }
    window.onload = initWebSocket;
    
    function setMode(m) { ws.send(JSON.stringify({'action':'mode', 'value':m})); }
    function setShape(s) { ws.send(JSON.stringify({'action':'shape', 'value':s})); }
    function sendColor(target, hex) { ws.send(JSON.stringify({'action':'colorTarget', 'target':target, 'value':hex})); }
    function sendText() {
      var val = document.getElementById('textInput').value;
      ws.send(JSON.stringify({'action':'text', 'value':val}));
    }
    function sendAudio(val) {
      ws.send(JSON.stringify({'action':'audio', 'value':parseInt(val)}));
    }
  </script>
</body>
</html>
)rawliteral";

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) {
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, payload)) return;

    String action = doc["action"];

    if (action == "mode") {
      String value = doc["value"];
      if (value == "clock") currentMode = MODE_CLOCK;
      else if (value == "3d") currentMode = MODE_3D_OBJECT;
      else if (value == "lyrics") currentMode = MODE_LYRICS;
      else if (value == "text") currentMode = MODE_CUSTOM_TEXT;
      else if (value == "vu") currentMode = MODE_VU_METER;
      tft.fillScreen(backgroundColor);
      lastSec = -1;
    } 
    else if (action == "shape") {
      currentMode = MODE_3D_OBJECT;
      String value = doc["value"];
      if (value == "hypercube") currentShape = SHAPE_HYPERCUBE;
      else if (value == "gyro") currentShape = SHAPE_GYRO;
      else if (value == "gem") currentShape = SHAPE_COMPLEX_GEM;
      else if (value == "icosahedron") currentShape = SHAPE_ICOSAHEDRON;
      else if (value == "torus") currentShape = SHAPE_TORUS;
      tft.fillScreen(backgroundColor);
    }
    else if (action == "colorTarget") {
      String target = doc["target"];
      uint16_t color = hexTo565(doc["value"].as<String>());
      
      if (target == "primary") primaryTextColor = color;
      else if (target == "accent") accentColor = color;
      else if (target == "bg") {
        backgroundColor = color;
        tft.fillScreen(backgroundColor);
      }
      lastSec = -1;
    }
    else if (action == "text") {
      currentMode = MODE_CUSTOM_TEXT;
      customText = doc["value"].as<String>();
      lyricScrollY = 128;
      tft.fillScreen(backgroundColor);
    }
    else if (action == "audio") {
      currentAudioLevel = doc["value"].as<int>();
    }
  }
}

// --- Render Engine Functions ---

void renderClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (lastSec == -1) {
    tft.fillScreen(backgroundColor);
    tft.drawFastHLine(4, 4, 25, accentColor);
    tft.drawFastVLine(4, 4, 21, accentColor);
    tft.drawFastHLine(131, 4, 25, accentColor);
    tft.drawFastVLine(155, 4, 21, accentColor);
    
    tft.drawFastHLine(4, 123, 25, accentColor);
    tft.drawFastVLine(4, 102, 21, accentColor);
    tft.drawFastHLine(131, 123, 25, accentColor);
    tft.drawFastVLine(155, 102, 21, accentColor);
  }

  char timeStr[6], secStr[3], dateStr[12];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  strftime(secStr, sizeof(secStr), "%S", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%a %d %b", &timeinfo);

  if (timeinfo.tm_sec != lastSec) {
    tft.fillRect(20, 35, 90, 30, backgroundColor);
    tft.fillRect(115, 42, 35, 20, backgroundColor);
    tft.fillRect(45, 75, 80, 15, backgroundColor);

    tft.setTextSize(3);
    tft.setTextColor(primaryTextColor);
    tft.setCursor(20, 35);
    tft.print(timeStr);

    tft.setTextSize(2);
    tft.setTextColor(accentColor);
    tft.setCursor(115, 42);
    tft.print(secStr);

    tft.setTextSize(1);
    tft.setTextColor(primaryTextColor);
    tft.setCursor(45, 75);
    tft.print(dateStr);

    lastSec = timeinfo.tm_sec;
  }
}

void render3DObject() {
  tft.fillScreen(backgroundColor);

  float radX = angleX * 0.01745329;
  float radY = angleY * 0.01745329;
  float radZ = angleZ * 0.01745329;

  if (currentShape == SHAPE_HYPERCUBE) {
    float cube[8][3] = {
      {-18,-18,-18}, {18,-18,-18}, {18,18,-18}, {-18,18,-18},
      {-18,-18,18},  {18,-18,18},  {18,18,18},  {-18,18,18}
    };
    float proj[8][2];

    for (int i = 0; i < 8; i++) {
      float y1 = cube[i][1] * cos(radX) - cube[i][2] * sin(radX);
      float z1 = cube[i][1] * sin(radX) + cube[i][2] * cos(radX);
      float x2 = cube[i][0] * cos(radY) + z1 * sin(radY);
      float z2 = -cube[i][0] * sin(radY) + z1 * cos(radY);
      float x3 = x2 * cos(radZ) - y1 * sin(radZ);
      float y3 = x2 * sin(radZ) + y1 * cos(radZ);

      proj[i][0] = x3 + 80;
      proj[i][1] = y3 + 64;
    }

    int edges[12][2] = {
      {0,1},{1,2},{2,3},{3,0},
      {4,5},{5,6},{6,7},{7,4},
      {0,4},{1,5},{2,6},{3,7}
    };

    for (int i = 0; i < 12; i++) {
      tft.drawLine(proj[edges[i][0]][0], proj[edges[i][0]][1], proj[edges[i][1]][0], proj[edges[i][1]][1], primaryTextColor);
    }
  }
  else if (currentShape == SHAPE_GYRO) {
    int r1 = 26, r2 = 18;
    for (int i = 0; i < 360; i += 20) {
      float rad = i * 0.01745329;
      float x1 = r1 * cos(rad), y1 = r1 * sin(rad);
      float x2 = r2 * cos(rad + radX), z2 = r2 * sin(rad + radX);

      tft.drawPixel(x1 + 80, y1 + 64, primaryTextColor);
      tft.drawPixel(x2 + 80, z2 + 64, accentColor);
    }
  }
  else if (currentShape == SHAPE_COMPLEX_GEM) {
    float nodes[6][3] = {{0,-28,0}, {0,28,0}, {-22,0,0}, {22,0,0}, {0,0,-22}, {0,0,22}};
    float proj[6][2];

    for (int i = 0; i < 6; i++) {
      float y1 = nodes[i][1] * cos(radX) - nodes[i][2] * sin(radX);
      float z1 = nodes[i][1] * sin(radX) + nodes[i][2] * cos(radX);
      float x2 = nodes[i][0] * cos(radY) + z1 * sin(radY);

      proj[i][0] = x2 + 80;
      proj[i][1] = y1 + 64;
    }

    int edges[12][2] = {
      {0,2},{0,3},{0,4},{0,5},
      {1,2},{1,3},{1,4},{1,5},
      {2,4},{4,3},{3,5},{5,2}
    };

    for (int i = 0; i < 12; i++) {
      tft.drawLine(proj[edges[i][0]][0], proj[edges[i][0]][1], proj[edges[i][1]][0], proj[edges[i][1]][1], accentColor);
    }
  }
  else if (currentShape == SHAPE_ICOSAHEDRON) {
    const float phi = (1.0 + sqrt(5.0)) / 2.0;
    float scale = 14.0;
    float ico[12][3] = {
      {-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0},
      {0, -1, phi}, {0, 1, phi}, {0, -1, -phi}, {0, 1, -phi},
      {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1}
    };
    int edges[30][2] = {
      {0,11},{0,5},{0,1},{0,7},{0,10},{1,7},{1,8},{1,9},{1,5},{2,3},
      {2,4},{2,11},{2,10},{2,6},{3,4},{3,6},{3,8},{3,9},{4,5},{4,9},
      {5,11},{6,7},{6,8},{7,10},{8,9},{10,11},{4,11},{5,9},{6,10},{7,8}
    };

    float proj[12][2];
    for (int i = 0; i < 12; i++) {
      float x = ico[i][0] * scale, y = ico[i][1] * scale, z = ico[i][2] * scale;
      float y1 = y * cos(radX) - z * sin(radX);
      float z1 = y * sin(radX) + z * cos(radX);
      float x2 = x * cos(radY) + z1 * sin(radY);
      float z2 = -x * sin(radY) + z1 * cos(radY);
      float x3 = x2 * cos(radZ) - y1 * sin(radZ);
      float y3 = x2 * sin(radZ) + y1 * cos(radZ);

      proj[i][0] = x3 + 80;
      proj[i][1] = y3 + 64;
    }

    for (int i = 0; i < 30; i++) {
      tft.drawLine(proj[edges[i][0]][0], proj[edges[i][0]][1], proj[edges[i][1]][0], proj[edges[i][1]][1], primaryTextColor);
    }
  } 
  else if (currentShape == SHAPE_TORUS) {
    float R = 22.0, r = 8.0;
    int segU = 12, segV = 8;
    
    for (int i = 0; i < segU; i++) {
      float u1 = i * (2.0 * M_PI / segU);
      float u2 = (i + 1) * (2.0 * M_PI / segU);
      
      for (int j = 0; j < segV; j++) {
        float v1 = j * (2.0 * M_PI / segV);

        float x = (R + r * cos(v1)) * cos(u1);
        float y = (R + r * cos(v1)) * sin(u1);
        float z = r * sin(v1);

        float x_next = (R + r * cos(v1)) * cos(u2);
        float y_next = (R + r * cos(v1)) * sin(u2);
        float z_next = r * sin(v1);

        float y1 = y * cos(radX) - z * sin(radX);
        float z1 = y * sin(radX) + z * cos(radX);
        float x2 = x * cos(radY) + z1 * sin(radY);

        float y1_n = y_next * cos(radX) - z_next * sin(radX);
        float z1_n = y_next * sin(radX) + z_next * cos(radX);
        float x2_n = x_next * cos(radY) + z1_n * sin(radY);

        tft.drawLine(x2 + 80, y1 + 64, x2_n + 80, y1_n + 64, accentColor);
      }
    }
  }

  angleX += 2.0;
  angleY += 3.0;
  angleZ += 1.0;
}

void renderCustomText() {
  if (millis() - lastScrollTime > 30) {
    tft.fillScreen(backgroundColor);
    tft.setTextSize(2);
    tft.setTextColor(primaryTextColor);
    tft.setCursor(10, lyricScrollY);
    tft.print(customText);

    lyricScrollY -= 2;
    if (lyricScrollY < -20) lyricScrollY = 128;

    lastScrollTime = millis();
  }
}

void renderLyrics() {
  if (millis() - lastScrollTime > 40) {
    tft.fillScreen(backgroundColor);
    tft.setTextSize(1);
    tft.setTextColor(accentColor);
    tft.setCursor(15, lyricScrollY);
    tft.print(currentLyric);

    lyricScrollY -= 1;
    if (lyricScrollY < -15) lyricScrollY = 128;

    lastScrollTime = millis();
  }
}

void renderVUMeter() {
  tft.fillScreen(backgroundColor);
  
  int barWidth = 12;
  int numBars = 8;
  int startX = 18;
  
  for (int i = 0; i < numBars; i++) {
    int barHeight = map(currentAudioLevel, 0, 100, 5, 100) + (sin(i + millis()*0.005) * 10);
    barHeight = constrain(barHeight, 4, 100);
    
    uint16_t color = primaryTextColor;
    if (barHeight > 70) color = ST7735_RED;
    else if (barHeight > 40) color = accentColor;
    
    tft.fillRect(startX + (i * (barWidth + 4)), 110 - barHeight, barWidth, barHeight, color);
    tft.drawRect(startX + (i * (barWidth + 4)), 110 - barHeight, barWidth, barHeight, ST7735_WHITE);
  }
  
  tft.setTextSize(1);
  tft.setTextColor(primaryTextColor);
  tft.setCursor(50, 118);
  tft.print("AUDIO VU");
}

// --- AP Mode Fallback Setup ---
void startAPMode() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_RED);
  tft.setCursor(10, 20);
  tft.print("WiFi Failed!");
  
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 45);
  tft.print("Connect phone to:");
  tft.setTextColor(ST7735_YELLOW);
  tft.setCursor(10, 60);
  tft.print("XIAO-Hologram-Setup");

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 85);
  tft.print("Open browser at:");
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(10, 100);
  tft.print("192.168.4.1");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("XIAO-Hologram-Setup");

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", config_html);
  });

  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("pass");

    preferences.begin("wifi_config", false);
    preferences.putString("ssid", newSSID);
    preferences.putString("pass", newPass);
    preferences.end();

    server.send(200, "text/html", "<h2>Saved! Rebooting device...</h2>");
    delay(2000);
    ESP.restart();
  });

  server.begin();

  while (true) {
    server.handleClient();
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); 
  applyHorizontalMirror();
  tft.fillScreen(backgroundColor);

  // Read saved Wi-Fi details from NVS storage
  preferences.begin("wifi_config", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("pass", "");
  preferences.end();

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 50);
  tft.print("Connecting WiFi...");

  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
  }

  // If Wi-Fi fails or no credentials saved, trigger AP Captive Setup
  if (WiFi.status() != WL_CONNECTED) {
    startAPMode();
  }

  tft.fillScreen(backgroundColor);
  tft.setCursor(10, 50);
  tft.print("IP: ");
  tft.print(WiFi.localIP());
  delay(1500);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", index_html); });
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  server.handleClient();
  webSocket.loop();

  switch (currentMode) {
    case MODE_CLOCK:
      renderClock();
      delay(50);
      break;
    case MODE_3D_OBJECT:
      render3DObject();
      delay(20);
      break;
    case MODE_CUSTOM_TEXT:
      renderCustomText();
      break;
    case MODE_LYRICS:
      renderLyrics();
      break;
    case MODE_VU_METER:
      renderVUMeter();
      delay(30);
      break;
  }
}