/*
 * GPS Speedometer - ESP32-S3-DevKitC-1
 * Uses Adafruit_ST7735 (verified working on this hardware)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>

#define GPS_RX_PIN    6
#define GPS_TX_PIN    5
#define TFT_BL_PIN    7
#define TFT_CS_PIN    10
#define TFT_DC_PIN    9
#define TFT_RST_PIN   8
#define TFT_MOSI_PIN  11
#define TFT_SCLK_PIN  12

const char* AP_SSID = "GPS-Speedometer";
const char* AP_PASS = "12345678";
IPAddress AP_IP(192, 168, 1, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

// ===================== Settings =====================
struct Settings {
  uint16_t bgColor        = 0x0000; // Black
  uint16_t speedColor     = 0xFFFF; // White
  uint16_t unitColor      = 0x07FF; // Cyan
  uint16_t labelColor     = 0x07E0; // Green
  uint16_t satColor       = 0xFFE0; // Yellow
  uint16_t maxSpeedColor  = 0xF800; // Red
  uint16_t accentColor    = 0x07E0; // Green
  bool useKmh             = true;
  float maxSpeed          = 120.0;
};

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
Settings cfg;

float currentSpeed   = 0.0;
float maxSpeedSeen   = 0.0;
int   satellites     = 0;
bool  gpsFixed       = false;
float latitude       = 0.0;
float longitude      = 0.0;
unsigned long lastGpsUpdate = 0;
unsigned long lastTftUpdate = 0;
int lastSpeed = -999;

// ===================== Satellite Skyview Data =====================
#define MAX_SATELLITES 16

struct SkySat {
  int    id   = 0;
  float  azimuth   = 0;
  float  elevation = 0;
  float  snr   = 0;
  bool   used  = false;
};

SkySat skySats[MAX_SATELLITES];
int skySatCount = 0;

// TinyGPSCustom for GSV satellite data
TinyGPSCustom customGsvTotal;
TinyGPSCustom customGsvMsgNum;
TinyGPSCustom customGsvSatsInView;
TinyGPSCustom customSatData[64];

// ===================== Utility Functions =====================
uint16_t hexToRgb565(String hex) {
  if (hex.startsWith("#")) hex = hex.substring(1);
  long val = strtol(hex.c_str(), nullptr, 16);
  uint8_t r = (val >> 16) & 0xFF;
  uint8_t g = (val >> 8)  & 0xFF;
  uint8_t b = val & 0xFF;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

String rgb565ToHex(uint16_t color) {
  uint8_t r = ((color >> 11) & 0x1F) << 3;
  uint8_t g = ((color >> 5)  & 0x3F) << 2;
  uint8_t b = (color & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}

void saveSettings() {
  prefs.begin("speedcfg", false);
  prefs.putUShort("bgColor",     cfg.bgColor);
  prefs.putUShort("speedColor",  cfg.speedColor);
  prefs.putUShort("unitColor",   cfg.unitColor);
  prefs.putUShort("labelColor",  cfg.labelColor);
  prefs.putUShort("satColor",    cfg.satColor);
  prefs.putUShort("maxSpdColor", cfg.maxSpeedColor);
  prefs.putUShort("accentColor", cfg.accentColor);
  prefs.putBool  ("useKmh",      cfg.useKmh);
  prefs.putFloat ("maxSpeed",    cfg.maxSpeed);
  prefs.end();
}

void loadSettings() {
  prefs.begin("speedcfg", true);
  cfg.bgColor       = prefs.getUShort("bgColor",     cfg.bgColor);
  cfg.speedColor    = prefs.getUShort("speedColor",  cfg.speedColor);
  cfg.unitColor     = prefs.getUShort("unitColor",   cfg.unitColor);
  cfg.labelColor    = prefs.getUShort("labelColor",  cfg.labelColor);
  cfg.satColor      = prefs.getUShort("satColor",    cfg.satColor);
  cfg.maxSpeedColor = prefs.getUShort("maxSpdColor", cfg.maxSpeedColor);
  cfg.accentColor   = prefs.getUShort("accentColor", cfg.accentColor);
  cfg.useKmh        = prefs.getBool  ("useKmh",      cfg.useKmh);
  cfg.maxSpeed      = prefs.getFloat ("maxSpeed",    cfg.maxSpeed);
  prefs.end();
}

// ===================== Drawing Functions =====================

void drawSpeedBar(float speed, float maxSpd) {
  const int BAR_X = 2;
  const int BAR_Y = 110;
  const int BAR_W = 124;
  const int BAR_H = 12;
  const int RADIUS = 4;

  tft.fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, RADIUS, 0x2104);

  float ratio = constrain(speed / maxSpd, 0.0, 1.0);
  int fillW = (int)(ratio * BAR_W);
  if (fillW < 1 && speed > 0) fillW = 1;

  for (int x = 0; x < fillW; x++) {
    float t = (float)x / BAR_W;
    uint8_t r, g, b;
    if (t < 0.5) {
      float tt = t * 2.0;
      r = (uint8_t)(tt * 255);
      g = 200;
      b = 0;
    } else {
      float tt = (t - 0.5) * 2.0;
      r = 255;
      g = (uint8_t)((1.0 - tt) * 200);
      b = 0;
    }
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    tft.drawFastVLine(BAR_X + x, BAR_Y + 1, BAR_H - 2, c);
  }

  for (int pct : {25, 50, 75}) {
    int tx = BAR_X + (int)(pct * BAR_W / 100);
    tft.drawFastVLine(tx, BAR_Y, BAR_H, 0x4208);
  }
}

void drawSatIcon(int x, int y, int sats, bool fixed) {
  uint16_t col = fixed ? cfg.accentColor : cfg.satColor;
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 2;
    int bx = x + i * 5;
    int by = y + (8 - bh);
    if (i < (sats / 3 + (sats > 0 ? 1 : 0))) {
      tft.fillRect(bx, by, 3, bh, col);
    } else {
      tft.drawRect(bx, by, 3, bh, 0x4208);
    }
  }
}

void drawSpeedometer() {
  float displaySpeed = currentSpeed;
  if (!cfg.useKmh) displaySpeed *= 0.621371;

  float maxDisp  = cfg.useKmh ? cfg.maxSpeed : cfg.maxSpeed * 0.621371;

  // Top header bar
  tft.fillRect(0, 0, 128, 22, 0x1082);
  tft.fillCircle(8, 11, 4, gpsFixed ? cfg.accentColor : 0xF800);
  drawSatIcon(16, 3, satellites, gpsFixed);

  tft.setTextColor(cfg.satColor, 0x1082);
  tft.setTextSize(1);
  tft.setCursor(38, 7);
  tft.print(satellites);
  tft.print(" sats");

  float maxD = cfg.useKmh ? maxSpeedSeen : maxSpeedSeen * 0.621371;
  tft.setTextColor(cfg.maxSpeedColor, 0x1082);
  tft.setCursor(80, 3);
  tft.setTextSize(1);
  tft.print("MAX");
  tft.setCursor(80, 13);
  char mxbuf[8];
  snprintf(mxbuf, sizeof(mxbuf), "%.0f", maxD);
  tft.print(mxbuf);

  // Main speed area
  tft.fillRect(0, 23, 128, 85, cfg.bgColor);

  tft.setTextColor(cfg.labelColor, cfg.bgColor);
  tft.setTextSize(1);
  tft.setCursor(44, 27);
  tft.print("SPEED");

  char spdbuf[6];
  snprintf(spdbuf, sizeof(spdbuf), "%.0f", displaySpeed);
  int spd_int = (int)displaySpeed;

  tft.setTextColor(cfg.speedColor, cfg.bgColor);
  if (spd_int < 10) {
    tft.setTextSize(6);
    tft.setCursor(42, 37);
  } else if (spd_int < 100) {
    tft.setTextSize(6);
    tft.setCursor(18, 37);
  } else {
    tft.setTextSize(5);
    tft.setCursor(8, 42);
  }
  tft.print(spdbuf);

  tft.setTextColor(cfg.unitColor, cfg.bgColor);
  tft.setTextSize(2);
  tft.setCursor(34, 91);
  if (cfg.useKmh) {
    tft.write('k'); tft.write('m'); tft.write('/'); tft.write('h');
  } else {
    tft.write('m'); tft.write('p'); tft.write('h');
  }

  drawSpeedBar(displaySpeed, maxDisp);
}

// ===================== Satellite Skyview on TFT =====================
static const int SKY_CX = 64;
static const int SKY_CY = 11;
static const int SKY_R  = 9;

void drawSkyView() {
  tft.fillCircle(SKY_CX, SKY_CY, SKY_R, 0x1082);
  tft.drawCircle(SKY_CX, SKY_CY, SKY_R, 0x4208);
  tft.drawCircle(SKY_CX, SKY_CY, SKY_R * 2 / 3, 0x2104);
  tft.drawCircle(SKY_CX, SKY_CY, SKY_R / 3, 0x2104);
  tft.drawFastHLine(SKY_CX - SKY_R, SKY_CY, SKY_R * 2, 0x2104);
  tft.drawFastVLine(SKY_CX, SKY_CY - SKY_R, SKY_R * 2, 0x2104);

  tft.setTextColor(0x4208, 0x1082);
  tft.setTextSize(1);
  tft.drawChar(SKY_CX - 1, SKY_CY - SKY_R - 5, 'N', 0x4208, 0x1082, 1);
  tft.drawChar(SKY_CX - 1, SKY_CY + SKY_R - 1, 'S', 0x4208, 0x1082, 1);
  tft.drawChar(SKY_CX + SKY_R - 2, SKY_CY - 1, 'E', 0x4208, 0x1082, 1);
  tft.drawChar(SKY_CX - SKY_R - 3, SKY_CY - 1, 'W', 0x4208, 0x1082, 1);

  for (int i = 0; i < skySatCount; i++) {
    SkySat& s = skySats[i];
    if (s.elevation < 0) continue;

    float r = SKY_R * (1.0 - s.elevation / 90.0);
    float rad = radians(s.azimuth - 90.0);
    int sx = (int)(SKY_CX + cos(rad) * r);
    int sy = (int)(SKY_CY - sin(rad) * r);

    uint16_t col = cfg.accentColor;
    tft.fillCircle(sx, sy, 2, col);
  }
}

// ===================== Web Server =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPS Speedometer</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d1117;color:#e6edf3;font-family:-apple-system,sans-serif;min-height:100vh;padding:20px}
  h1{text-align:center;font-size:1.5rem;font-weight:700;letter-spacing:.05em;color:#58a6ff;margin-bottom:4px}
  .sub{text-align:center;color:#8b949e;font-size:.8rem;margin-bottom:24px}
  .card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:18px;margin-bottom:14px}
  .card h2{font-size:.85rem;font-weight:600;color:#8b949e;letter-spacing:.1em;text-transform:uppercase;margin-bottom:14px}
  .row{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
  .row:last-child{margin-bottom:0}
  label{font-size:.9rem;color:#c9d1d9}
  input[type=color]{width:44px;height:30px;border:1px solid #30363d;border-radius:6px;cursor:pointer;background:none;padding:2px}
  .toggle{display:flex;background:#0d1117;border:1px solid #30363d;border-radius:8px;overflow:hidden}
  .toggle button{flex:1;padding:7px 0;border:none;background:none;color:#8b949e;font-size:.85rem;cursor:pointer;transition:.2s}
  .toggle button.active{background:#1f6feb;color:#fff}
  .speed-row{display:flex;align-items:center;gap:10px}
  input[type=range]{flex:1;accent-color:#1f6feb}
  .speed-val{min-width:64px;text-align:right;font-size:.85rem;color:#58a6ff;font-weight:600}
  .status{display:flex;gap:16px;padding:14px;background:#0d1117;border-radius:8px}
  .stat{flex:1;text-align:center}
  .stat .n{font-size:2rem;font-weight:700;color:#58a6ff;line-height:1}
  .stat .l{font-size:.72rem;color:#8b949e;margin-top:4px;text-transform:uppercase;letter-spacing:.08em}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .dot.ok{background:#2ea043}.dot.no{background:#f85149}
  .btn-save{width:100%;padding:12px;background:linear-gradient(135deg,#1f6feb,#58a6ff);color:#fff;border:none;border-radius:8px;font-size:1rem;font-weight:600;cursor:pointer;margin-top:4px}
  .btn-reset{width:100%;padding:9px;background:none;color:#f85149;border:1px solid #f85149;border-radius:8px;font-size:.85rem;cursor:pointer;margin-top:8px}
  .toast{position:fixed;top:20px;left:50%;transform:translateX(-50%);background:#2ea043;color:#fff;padding:10px 22px;border-radius:8px;font-size:.9rem;display:none;z-index:99}
  .bar-preview{height:14px;border-radius:6px;background:linear-gradient(to right,#00c800,#ffd000,#ff3000);margin-top:10px;position:relative}
  .bar-needle{position:absolute;top:-3px;width:3px;height:20px;background:#fff;border-radius:2px;transition:.3s}
  #skyview-canvas{display:block;margin:0 auto;background:#0d1117;border-radius:8px}
  .sky-list{max-height:200px;overflow-y:auto;margin-top:10px;font-size:.78rem}
  .sky-list table{width:100%;border-collapse:collapse}
  .sky-list th{color:#8b949e;text-align:left;padding:4px 6px;border-bottom:1px solid #30363d}
  .sky-list td{padding:3px 6px;border-bottom:1px solid #21262d}
  .sky-list .used{color:#2ea043}
  .sky-list .not-used{color:#8b949e}
</style>
</head>
<body>
<h1>GPS Speedometer</h1>
<div class="sub">ESP32-S3 · ST7735 · ATGM336H</div>

<div class="card">
  <h2>Status</h2>
  <div class="status">
    <div class="stat">
      <div class="n" id="spd">--</div>
      <div class="l" id="unit-label">km/h</div>
    </div>
    <div class="stat">
      <div class="n" id="sats">--</div>
      <div class="l">Sats</div>
    </div>
    <div class="stat">
      <div class="n" id="maxspd">--</div>
      <div class="l">Max Speed</div>
    </div>
  </div>
  <div style="margin-top:10px;font-size:.8rem;color:#8b949e">
    <span class="dot" id="fix-dot"></span><span id="fix-text">Waiting GPS signal...</span>
  </div>
  <div class="bar-preview">
    <div class="bar-needle" id="bar-needle" style="left:0%"></div>
  </div>
</div>

<div class="card">
  <h2>Skyview</h2>
  <canvas id="skyview-canvas" width="260" height="260"></canvas>
  <div class="sky-list">
    <table>
      <thead><tr><th>PRN</th><th>Azimuth</th><th>Elevation</th><th>SNR</th><th>Used</th></tr></thead>
      <tbody id="sky-table"></tbody>
    </table>
  </div>
</div>

<div class="card">
  <h2>Speed Unit</h2>
  <div class="toggle">
    <button id="btn-kmh" class="active" onclick="setUnit(true)">km/h</button>
    <button id="btn-mph" onclick="setUnit(false)">mph</button>
  </div>
</div>

<div class="card">
  <h2>Bar Max Speed</h2>
  <div class="speed-row">
    <input type="range" id="max-speed" min="60" max="300" step="10" value="120" oninput="updateMaxSpd(this.value)">
    <span class="speed-val" id="max-speed-val">120 km/h</span>
  </div>
</div>

<div class="card">
  <h2>Colors</h2>
  <div class="row"><label>Background</label><input type="color" id="c-bg" value="#000000"></div>
  <div class="row"><label>Speed</label><input type="color" id="c-speed" value="#FFFFFF"></div>
  <div class="row"><label>Unit</label><input type="color" id="c-unit" value="#00FFFF"></div>
  <div class="row"><label>Label</label><input type="color" id="c-label" value="#888888"></div>
  <div class="row"><label>Satellite</label><input type="color" id="c-sat" value="#FFE000"></div>
  <div class="row"><label>Max Speed</label><input type="color" id="c-maxspd" value="#FF0000"></div>
  <div class="row"><label>GPS Fix</label><input type="color" id="c-accent" value="#00FF00"></div>
</div>

<button class="btn-save" onclick="saveSettings()">Save Settings</button>
<button class="btn-reset" onclick="resetMax()">Reset Max Speed</button>

<div class="toast" id="toast"></div>

<script>
let useKmh = true;

function setUnit(kmh) {
  useKmh = kmh;
  document.getElementById('btn-kmh').classList.toggle('active', kmh);
  document.getElementById('btn-mph').classList.toggle('active', !kmh);
  document.getElementById('unit-label').textContent = kmh ? 'km/h' : 'mph';
  updateMaxSpd(document.getElementById('max-speed').value);
}

function updateMaxSpd(v) {
  document.getElementById('max-speed-val').textContent = v + ' ' + (useKmh ? 'km/h' : 'mph');
}

function toast(msg, err) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.style.background = err ? '#f85149' : '#2ea043';
  t.style.display = 'block';
  setTimeout(() => t.style.display = 'none', 2500);
}

function saveSettings() {
  const payload = {
    useKmh: useKmh,
    maxSpeed: parseInt(document.getElementById('max-speed').value),
    bgColor:       document.getElementById('c-bg').value,
    speedColor:    document.getElementById('c-speed').value,
    unitColor:     document.getElementById('c-unit').value,
    labelColor:    document.getElementById('c-label').value,
    satColor:      document.getElementById('c-sat').value,
    maxSpeedColor: document.getElementById('c-maxspd').value,
    accentColor:   document.getElementById('c-accent').value,
  };
  fetch('/api/settings', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(payload)
  }).then(r => r.json()).then(d => {
    toast(d.ok ? 'Settings saved!' : 'Save failed', !d.ok);
  }).catch(() => toast('Network error', true));
}

function resetMax() {
  fetch('/api/reset-max', {method: 'POST'})
    .then(() => toast('Max speed reset'));
}

function drawSkyview(satellites) {
  const canvas = document.getElementById('skyview-canvas');
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  const cx = W / 2, cy = H / 2;
  const R = Math.min(W, H) / 2 - 10;

  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#0d1117';
  ctx.fillRect(0, 0, W, H);

  ctx.strokeStyle = '#30363d';
  ctx.lineWidth = 1;
  for (let i = 1; i <= 3; i++) {
    ctx.beginPath();
    ctx.arc(cx, cy, R * i / 3, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.beginPath();
  ctx.moveTo(cx - R, cy); ctx.lineTo(cx + R, cy);
  ctx.moveTo(cx, cy - R); ctx.lineTo(cx, cy + R);
  ctx.stroke();

  ctx.fillStyle = '#8b949e';
  ctx.font = '12px sans-serif';
  ctx.textAlign = 'center';
  ctx.fillText('N', cx, cy - R - 5);
  ctx.fillText('S', cx, cy + R + 14);
  ctx.fillText('E', cx + R + 10, cy + 4);
  ctx.fillText('W', cx - R - 10, cy + 4);

  for (const sat of satellites) {
    const az = sat.azimuth || 0;
    const el = sat.elevation || 0;
    const r = R * (1.0 - el / 90.0);
    const rad = Math.PI * (az - 90) / 180;
    const sx = cx + Math.cos(rad) * r;
    const sy = cy - Math.sin(rad) * r;

    ctx.beginPath();
    ctx.arc(sx, sy, 5, 0, Math.PI * 2);
    ctx.fillStyle = sat.used ? '#2ea043' : '#ffe000';
    ctx.fill();

    ctx.fillStyle = '#e6edf3';
    ctx.font = '10px monospace';
    ctx.textAlign = 'left';
    ctx.fillText(sat.id, sx + 7, sy + 3);
  }
}

function updateStatus() {
  fetch('/api/status').then(r => r.json()).then(d => {
    document.getElementById('spd').textContent = d.speed.toFixed(1);
    document.getElementById('sats').textContent = d.satellites;
    document.getElementById('maxspd').textContent = d.maxSpeed.toFixed(1);
    document.getElementById('unit-label').textContent = d.useKmh ? 'km/h' : 'mph';

    const fixed = d.gpsFixed;
    document.getElementById('fix-dot').className = 'dot ' + (fixed ? 'ok' : 'no');
    document.getElementById('fix-text').textContent = fixed
      ? 'GPS locked - ' + d.satellites + ' sats'
      : 'Waiting GPS signal...';

    const ratio = Math.min(d.speed / d.maxBarSpeed, 1.0) * 100;
    document.getElementById('bar-needle').style.left = ratio + '%';

    drawSkyview(d.skyview || []);

    const tbody = document.getElementById('sky-table');
    tbody.innerHTML = '';
    if (d.skyview) {
      for (const sat of d.skyview) {
        const tr = document.createElement('tr');
        tr.innerHTML = '<td>' + sat.id + '</td>' +
          '<td>' + (sat.azimuth||0).toFixed(0) + '</td>' +
          '<td>' + (sat.elevation||0).toFixed(0) + '</td>' +
          '<td>' + (sat.snr||0).toFixed(1) + '</td>' +
          '<td class="' + (sat.used ? 'used' : 'not-used') + '">' + (sat.used ? 'Yes' : 'No') + '</td>';
        tbody.appendChild(tr);
      }
    }
  }).catch(() => {});
}

fetch('/api/settings').then(r => r.json()).then(d => {
  setUnit(d.useKmh);
  document.getElementById('max-speed').value = d.maxSpeed;
  updateMaxSpd(d.maxSpeed);
  document.getElementById('c-bg').value     = d.bgColor;
  document.getElementById('c-speed').value  = d.speedColor;
  document.getElementById('c-unit').value   = d.unitColor;
  document.getElementById('c-label').value  = d.labelColor;
  document.getElementById('c-sat').value    = d.satColor;
  document.getElementById('c-maxspd').value = d.maxSpeedColor;
  document.getElementById('c-accent').value = d.accentColor;
});

updateStatus();
setInterval(updateStatus, 1000);
</script>
</body>
</html>
)rawliteral";

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    float dispSpeed  = cfg.useKmh ? currentSpeed : currentSpeed * 0.621371;
    float dispMax    = cfg.useKmh ? maxSpeedSeen : maxSpeedSeen * 0.621371;
    float dispBarMax = cfg.useKmh ? cfg.maxSpeed  : cfg.maxSpeed * 0.621371;

    StaticJsonDocument<2048> doc;
    doc["speed"]       = dispSpeed;
    doc["maxSpeed"]    = dispMax;
    doc["maxBarSpeed"] = dispBarMax;
    doc["satellites"]  = satellites;
    doc["gpsFixed"]    = gpsFixed;
    doc["useKmh"]      = cfg.useKmh;
    doc["lat"]         = latitude;
    doc["lng"]         = longitude;

    JsonArray sky = doc.createNestedArray("skyview");
    for (int i = 0; i < skySatCount; i++) {
      JsonObject obj = sky.createNestedObject();
      obj["id"]        = skySats[i].id;
      obj["azimuth"]   = skySats[i].azimuth;
      obj["elevation"] = skySats[i].elevation;
      obj["snr"]       = skySats[i].snr;
      obj["used"]      = true;
    }

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<512> doc;
    doc["useKmh"]        = cfg.useKmh;
    doc["maxSpeed"]      = (int)cfg.maxSpeed;
    doc["bgColor"]       = rgb565ToHex(cfg.bgColor);
    doc["speedColor"]    = rgb565ToHex(cfg.speedColor);
    doc["unitColor"]     = rgb565ToHex(cfg.unitColor);
    doc["labelColor"]    = rgb565ToHex(cfg.labelColor);
    doc["satColor"]      = rgb565ToHex(cfg.satColor);
    doc["maxSpeedColor"] = rgb565ToHex(cfg.maxSpeedColor);
    doc["accentColor"]   = rgb565ToHex(cfg.accentColor);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  AsyncCallbackJsonWebHandler* settingsHandler =
    new AsyncCallbackJsonWebHandler("/api/settings",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();
        if (obj.containsKey("useKmh"))        cfg.useKmh        = obj["useKmh"];
        if (obj.containsKey("maxSpeed"))       cfg.maxSpeed      = obj["maxSpeed"].as<float>();
        if (obj.containsKey("bgColor"))        cfg.bgColor       = hexToRgb565(obj["bgColor"].as<String>());
        if (obj.containsKey("speedColor"))     cfg.speedColor    = hexToRgb565(obj["speedColor"].as<String>());
        if (obj.containsKey("unitColor"))      cfg.unitColor     = hexToRgb565(obj["unitColor"].as<String>());
        if (obj.containsKey("labelColor"))     cfg.labelColor    = hexToRgb565(obj["labelColor"].as<String>());
        if (obj.containsKey("satColor"))       cfg.satColor      = hexToRgb565(obj["satColor"].as<String>());
        if (obj.containsKey("maxSpeedColor"))  cfg.maxSpeedColor = hexToRgb565(obj["maxSpeedColor"].as<String>());
        if (obj.containsKey("accentColor"))    cfg.accentColor   = hexToRgb565(obj["accentColor"].as<String>());

        saveSettings();
        tft.fillScreen(cfg.bgColor);
        drawSpeedometer();

        StaticJsonDocument<64> resp;
        resp["ok"] = true;
        String out;
        serializeJson(resp, out);
        req->send(200, "application/json", out);
      });
  server.addHandler(settingsHandler);

  server.on("/api/reset-max", HTTP_POST, [](AsyncWebServerRequest* req) {
    maxSpeedSeen = 0;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("/");
  });

  server.begin();
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== GPS Speedometer Start ===");

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  Serial.println("TFT backlight ON");

  // Manual reset
  pinMode(TFT_RST_PIN, OUTPUT);
  digitalWrite(TFT_RST_PIN, HIGH);
  delay(100);
  digitalWrite(TFT_RST_PIN, LOW);
  delay(100);
  digitalWrite(TFT_RST_PIN, HIGH);
  delay(200);
  Serial.println("TFT reset done");

  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  Serial.println("SPI started");

  tft.initR(INITR_BLACKTAB);
  Serial.println("TFT init done");

  tft.setRotation(0);
  tft.fillScreen(ST7735_BLACK);
  Serial.println("TFT fill black");

  loadSettings();
  Serial.println("Settings loaded");

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS serial started");

  // Initialize TinyGPSCustom for GSV satellite data
  customGsvTotal.begin(gps, "GSV", 1);
  customGsvMsgNum.begin(gps, "GSV", 2);
  customGsvSatsInView.begin(gps, "GSV", 3);
  for (int i = 0; i < 64; i++) {
    customSatData[i].begin(gps, "GSV", 4 + i);
  }
  Serial.println("TinyGPSCustom initialized");

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println("WiFi AP started: " + String(AP_SSID));

  dnsServer.start(53, "*", AP_IP);
  setupWebServer();
  Serial.println("Web server started");

  Serial.print("IP: ");
  Serial.println(AP_IP);
  delay(2000);
  tft.fillScreen(cfg.bgColor);
}

// ===================== Loop =====================
void loop() {
  dnsServer.processNextRequest();

  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (gps.encode(c)) {
      lastGpsUpdate = millis();

      if (gps.satellites.isValid())
        satellites = gps.satellites.value();

      if (gps.location.isValid()) {
        gpsFixed  = true;
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
      }

      if (gps.speed.isValid()) {
        currentSpeed = gps.speed.kmph();
        if (currentSpeed > maxSpeedSeen) maxSpeedSeen = currentSpeed;
      }
    }
  }

  if (millis() - lastGpsUpdate > 5000) {
    gpsFixed     = false;
    currentSpeed = 0;
    satellites   = 0;
    skySatCount  = 0;
    lastSpeed    = -999;
  }

  // Process satellite skyview data
  bool gotGsvUpdate = false;
  for (int i = 0; i < 64; i++) {
    if (customSatData[i].isUpdated()) { gotGsvUpdate = true; break; }
  }
  if (gotGsvUpdate && satellites > 0) {
    skySatCount = 0;
    int numSats = min(satellites, MAX_SATELLITES);
    for (int s = 0; s < numSats; s++) {
      int base = s * 4;
      if (base + 0 < 64) skySats[skySatCount].id       = atoi(customSatData[base + 0].value());
      if (base + 1 < 64) skySats[skySatCount].azimuth   = atof(customSatData[base + 1].value());
      if (base + 2 < 64) skySats[skySatCount].elevation = atof(customSatData[base + 2].value());
      if (base + 3 < 64) skySats[skySatCount].snr       = atof(customSatData[base + 3].value());
      if (skySats[skySatCount].id > 0) {
        skySatCount++;
      }
    }
  }

  int dispSpeedInt = (int)currentSpeed;
  if (dispSpeedInt != lastSpeed) {
    lastSpeed = dispSpeedInt;
    if (millis() - lastTftUpdate > 300) {
      lastTftUpdate = millis();
      drawSpeedometer();
    }
  }
}
