#include <Adafruit_INA219.h>
#define DEBUG_MODE                                                             \
  0 // 1: Serial activo (Sondas -127), 0: Sondas activas (No Serial)
#include <DNSServer.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <OneWire.h>
#include <SPIMemory.h>
#include <U8g2lib.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <qrcode.h>

#ifdef ID
#undef ID
#endif
// --- PROTOTYPES ---
void handleConfig();
void handleData();
void handleDownload();
void handleWipe();
void handleDump();
void handleDashboard();
void handleHardware();
void handleDiag();
void handleSave();
void handleHistory();
void IRAM_ATTR handleEncoder();

// --- HARDWARE ---
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "south-america.pool.ntp.org", -10800);
OneWire oneWire(RX);
DallasTemperature sensors(&oneWire);
Adafruit_INA219 ina219;
SPIFlash flash(SS);

// --- ENCODER ---
#define ROTARY_ENCODER_A_PIN D3
#define ROTARY_ENCODER_B_PIN D4
#define ROTARY_ENCODER_BUTTON_PIN D0

volatile int encoderTicks = 0;
void IRAM_ATTR handleEncoder() {
  static uint8_t old_AB = 0;
  static const int8_t enc_states[] = {0,  -1, 1, 0, 1, 0, 0,  -1,
                                      -1, 0,  0, 1, 0, 1, -1, 0};
  old_AB <<= 2;
  old_AB |= (digitalRead(ROTARY_ENCODER_A_PIN) << 1) |
            digitalRead(ROTARY_ENCODER_B_PIN);
  encoderTicks += enc_states[(old_AB & 0x0f)];
}

// --- VARIABLES ---
float t1 = 0, t2 = 0, t3 = 0;
float vBus = 0, iAmps = 0, pWatts = 0;
int tMuestreoLocal = 5, tMuestreoCloud = 60;
String tsChannelID = "000000", tsAPIKey = "API_KEY";

// Historiales para las 3 sondas
int historialT1[128], historialT2[128], historialT3[128];
int sondaGraficada = 0;          // 0: Ambient, 1: Evap, 2: Cond
float yMin = -20.0, yMax = 50.0; // Límites dinámicos

bool monitoreoActivo = false, flashTemp = false;
uint32_t tiempoInicio = 0, lastCloudUpload = 0;
int lastHTTPResponse = 0;
const uint32_t WAIT_TIME = 10000;
uint32_t currentFlashAddr = 0;
String s1Alias = "Ambiente", s2Alias = "Evaporador", s3Alias = "Condensador";
int lastTSResponse = 0;
String lastTSUploadTime = "---";
uint32_t manID = 0;
uint8_t flashError = 0;
#include <SPI.h>

struct LogRecord {
  uint32_t timestamp;
  float temp1, temp2, temp3;
  float pW;
};

struct Config {
  uint32_t magic;
  int tml;
  int tmc;
  char tsid[16];
  char tskey[32];
  char s1a[32];
  char s2a[32];
  char s3a[32];
} settings;

void saveSettings() {
  settings.magic = 0xDEADBEEF;
  settings.tml = tMuestreoLocal;
  settings.tmc = tMuestreoCloud;
  memset(settings.tsid, 0, 16);
  strncpy(settings.tsid, tsChannelID.c_str(), 15);
  memset(settings.tskey, 0, 32);
  strncpy(settings.tskey, tsAPIKey.c_str(), 31);
  memset(settings.s1a, 0, 32);
  strncpy(settings.s1a, s1Alias.c_str(), 31);
  memset(settings.s2a, 0, 32);
  strncpy(settings.s2a, s2Alias.c_str(), 31);
  memset(settings.s3a, 0, 32);
  strncpy(settings.s3a, s3Alias.c_str(), 31);
  EEPROM.put(0, settings);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.get(0, settings);
  if (settings.magic == 0xDEADBEEF) {
    settings.tsid[15] = '\0';
    settings.tskey[31] = '\0';
    settings.s1a[31] = '\0';
    settings.s2a[31] = '\0';
    settings.s3a[31] = '\0';
    tMuestreoLocal = settings.tml;
    tMuestreoCloud = settings.tmc;
    tsChannelID = String(settings.tsid);
    tsAPIKey = String(settings.tskey);
    s1Alias = String(settings.s1a);
    s2Alias = String(settings.s2a);
    s3Alias = String(settings.s3a);
  }
}

// --- HELPERS ---
String getAddressStr(DeviceAddress da) {
  String out = "";
  for (uint8_t i = 0; i < 8; i++) {
    if (da[i] < 16)
      out += "0";
    out += String(da[i], HEX);
  }
  out.toUpperCase();
  return out;
}

// --- WEB UI HELPERS ---
void sendHeader(String title) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(
      "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>");
  server.sendContent(
      "<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
  server.sendContent("<title>OmniData | " + title + "</title>");
  server.sendContent(
      "<link href='https://fonts.googleapis.com/icon?family=Material+Icons' "
      "rel='stylesheet'>");
  server.sendContent("<style>:root{--bg:#0d1117; --side:#161b22; "
                     "--border:#30363d; --text:#c9d1d9; --accent:#58a6ff;} ");
  server.sendContent("body{margin:0; font-family:'Segoe UI',Roboto,sans-serif; "
                     "background:var(--bg); color:var(--text); display:flex; "
                     "min-height:100vh;} ");
  server.sendContent(
      ".sidebar{width:240px; background:var(--side); border-right:1px solid "
      "var(--border); display:flex; flex-direction:column; transition:0.3s; "
      "overflow:hidden; z-index:1000;} ");
  server.sendContent(
      ".sidebar h1{padding:20px 18px; font-size:1.2rem; color:var(--accent); "
      "margin:0; border-bottom:1px solid var(--border); display:flex; "
      "align-items:center; gap:10px; white-space:nowrap;} ");
  server.sendContent(
      ".nav-item{padding:15px 18px; color:var(--text); text-decoration:none; "
      "display:flex; align-items:center; gap:25px; border-bottom:1px solid "
      "rgba(255,255,255,0.05); transition:0.2s; white-space:nowrap; "
      "cursor:pointer;} ");
  server.sendContent(
      ".nav-item:hover{background:rgba(255,255,255,0.05); "
      "color:var(--accent);} .nav-item.active{background:rgba(88,166,255,0.1); "
      "color:var(--accent); border-right:3px solid var(--accent);} ");
  server.sendContent(".main-content{flex:1; padding:20px; overflow-y:auto; "
                     "overflow-x:hidden;} ");
  server.sendContent("@media (max-width:768px){ .sidebar{position:fixed; "
                     "left:0; width:60px; height:100%; top:0; "
                     "box-shadow:5px 0 15px rgba(0,0,0,0.3);} "
                     ".sidebar.open{width:240px;} "
                     ".sidebar h1 span:last-child{display:none;} "
                     ".sidebar.open h1 span:last-child{display:inline;} "
                     ".nav-item .nav-label{display:none;} "
                     ".sidebar.open .nav-item .nav-label{display:inline;} "
                     ".main-content{margin-left:60px; padding:15px;} } ");
  server.sendContent(
      ".card{background:var(--side); border:1px solid var(--border); "
      "border-radius:8px; padding:15px; margin-bottom:15px;} "
      ".grid{display:grid; grid-template-columns:repeat(auto-fit, "
      "minmax(200px, 1fr)); gap:15px;} ");
  server.sendContent(
      "h2{margin-top:0; color:var(--accent);} table{width:100%; "
      "border-collapse:collapse;} th,td{padding:12px; text-align:left; "
      "border-bottom:1px solid var(--border);} ");
  server.sendContent(".btn{padding:10px 20px; border-radius:5px; border:none; "
                     "cursor:pointer; font-weight:600; transition:0.2s; "
                     "text-decoration:none; display:inline-block;} ");
  server.sendContent(".btn-primary{background:var(--accent); color:#fff;} "
                     ".btn-danger{background:#f85149; color:#fff;} "
                     ".range-picker{background:var(--side); color:var(--text); "
                     "border:1px solid var(--border); "
                     "padding:5px 10px; border-radius:4px; margin-left:auto; "
                     "font-size:0.85rem; cursor:pointer;} "
                     "</style></head><body>");

  // Sidebar
  server.sendContent("<nav class='sidebar' id='sidebar'>");
  server.sendContent(
      "<div class='nav-item' onclick='toggleMenu()' "
      "style='background:rgba(88,166,255,0.1); color:var(--accent);'><span "
      "class='material-icons'>menu</span> <span "
      "class='nav-label'>Menu</span></div>");
  server.sendContent("<h1><span class='material-icons'>refrigerator</span> "
                     "<span>OmniData</span></h1>");
  server.sendContent("<a href='/' class='nav-item " +
                     String(title == "Dashboard" ? "active" : "") +
                     "'><span class='material-icons'>dashboard</span> <span "
                     "class='nav-label'>Dashboard</span></a>");
  server.sendContent("<a href='/config' class='nav-item " +
                     String(title == "Options" ? "active" : "") +
                     "'><span class='material-icons'>settings</span> <span "
                     "class='nav-label'>Options</span></a>");
  server.sendContent("<a href='/hardware' class='nav-item " +
                     String(title == "Sensors" ? "active" : "") +
                     "'><span class='material-icons'>router</span> <span "
                     "class='nav-label'>Sensors & IDs</span></a>");
  server.sendContent("<a href='/diag' class='nav-item " +
                     String(title == "Diag" ? "active" : "") +
                     "'><span class='material-icons'>terminal</span> <span "
                     "class='nav-label'>Diagnostics</span></a>");
  server.sendContent(
      "<div style='margin-top:auto; padding:20px; font-size:0.7rem; "
      "color:#8b949e;'>v2.5 Final Build</div></nav>");

  server.sendContent("<div class='main-content'>");
}

void sendFooter() {
  server.sendContent("<script>function toggleMenu(){ "
                     "document.getElementById('sidebar').classList.toggle('"
                     "open'); }</script>");
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

// --- WEB SERVER: CONFIGURACIÓN ---
void handleConfig() {
  sendHeader("Options");
  server.sendContent(
      "<h2>⚙️ Options & Configuration</h2><form action='/save' method='POST'>");
  server.sendContent("<div class='card'><h3>📡 Probe Aliases</h3>");
  server.sendContent("<div class='grid'><div class='card'>S1: <input "
                     "type='text' name='s1a' value='" +
                     s1Alias + "' style='width:100%'></div>");
  server.sendContent(
      "<div class='card'>S2: <input type='text' name='s2a' value='" + s2Alias +
      "' style='width:100%'></div>");
  server.sendContent(
      "<div class='card'>S3: <input type='text' name='s3a' value='" + s3Alias +
      "' style='width:100%'></div></div></div>");
  server.sendContent("<div class='card'><h3>⏱️ Sampling Intervals (sec)</h3>");
  server.sendContent("<div class='grid'><div>Local / Graph<br><input "
                     "type='number' name='tml' value='" +
                     String(tMuestreoLocal) + "' style='width:100%'></div>");
  server.sendContent(
      "<div>Cloud Sync<br><input type='number' name='tmc' value='" +
      String(tMuestreoCloud) + "' style='width:100%'></div></div></div>");
  server.sendContent("<div class='card'><h3>☁️ ThingSpeak Sync</h3>");
  server.sendContent("<div class='grid'><div>Channel ID<br><input type='text' "
                     "name='tsid' value='" +
                     tsChannelID + "' style='width:100%'></div>");
  server.sendContent(
      "<div>Write API Key<br><input type='text' name='tskey' value='" +
      tsAPIKey + "' style='width:100%'></div></div></div>");
  server.sendContent(
      "<button type='submit' class='btn btn-primary' style='width:100%; "
      "padding:20px;'>SAVE & APPLY CONFIG</button></form>");
  sendFooter();
}

void handleData() {
  String out = "{";
  out += "\"t1\":" + String(isfinite(t1) ? t1 : -127.0, 1) + ",";
  out += "\"t2\":" + String(isfinite(t2) ? t2 : -127.0, 1) + ",";
  out += "\"t3\":" + String(isfinite(t3) ? t3 : -127.0, 1) + ",";
  out += "\"vBus\":" + String(isfinite(vBus) ? vBus : 0.0, 2) + ",";
  out += "\"iAmps\":" + String(isfinite(iAmps) ? iAmps : 0.0, 3) + ",";
  out += "\"pWatts\":" + String(isfinite(pWatts) ? pWatts : 0.0, 2) + ",";
  out += "\"flashAddr\":" + String(currentFlashAddr) + ",";
  out += "\"flashError\":" + String(flashError) + ",";
  out += "\"jedec\":\"" + String(manID, HEX) + "\",";
  out += "\"tsRes\":" + String(lastTSResponse) + ",";
  out += "\"tsTime\":\"" + lastTSUploadTime + "\",";
  out += "\"s1a\":\"" + s1Alias + "\",";
  out += "\"s2a\":\"" + s2Alias + "\",";
  out += "\"s3a\":\"" + s3Alias + "\",";
  out += "\"nextCloud\":" +
         String((tMuestreoCloud * 1000 - (millis() - lastCloudUpload)) / 1000) +
         ",";
  out += "\"time\":\"" + timeClient.getFormattedTime() + "\"";
  out += "}";
  server.send(200, "application/json", out);
}

void handleHistory() {
  uint32_t hours = server.hasArg("h") ? server.arg("h").toInt() : 0;
  uint32_t maxPoints = 120;
  uint32_t startAddr = 0;
  uint32_t totalLogged = currentFlashAddr / sizeof(LogRecord);

  if (hours > 0) {
    uint32_t pointsToRead = (hours * 3600) / tMuestreoLocal;
    if (currentFlashAddr > pointsToRead * sizeof(LogRecord))
      startAddr = currentFlashAddr - pointsToRead * sizeof(LogRecord);
  } else {
    if (currentFlashAddr > 100 * sizeof(LogRecord))
      startAddr = currentFlashAddr - 100 * sizeof(LogRecord);
  }

  uint32_t totalPoints = (currentFlashAddr - startAddr) / sizeof(LogRecord);
  uint32_t skip = totalPoints / maxPoints;
  if (skip < 1)
    skip = 1;

  String out = "[";
  int count = 0;
  LogRecord rec;
  for (uint32_t addr = startAddr; addr < currentFlashAddr;
       addr += (skip * sizeof(LogRecord))) {
    if (flash.readAnything(addr, rec)) {
      if (count > 0)
        out += ",";
      out += "{\"t\":" + String(rec.timestamp) +
             ",\"t1\":" + String(rec.temp1, 1) +
             ",\"t2\":" + String(rec.temp2, 1) +
             ",\"t3\":" + String(rec.temp3, 1) + "}";
      count++;
      if (count >= maxPoints)
        break;
    }
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleDownload() {
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=omni_log.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("Time,T1,T2,T3,Watts\n");
  LogRecord rec;
  for (uint32_t addr = 0; addr < currentFlashAddr; addr += sizeof(LogRecord)) {
    if (flash.readAnything(addr, rec)) {
      String line = String(rec.timestamp) + "," + String(rec.temp1, 1) + "," +
                    String(rec.temp2, 1) + "," + String(rec.temp3, 1) + "," +
                    String(rec.pW, 1) + "\n";
      server.sendContent(line);
    }
  }
  server.sendContent("");
}

void handleWipe() {
  flash.eraseChip();
  currentFlashAddr = 0;
  server.send(200, "text/plain", "Memoria Formateada OK");
}

void handleDump() {
  server.sendHeader("Content-Type", "text/plain");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "--- MEMORY DUMP ---\n");
  server.sendContent("Addr\tTime\tT1\tT2\tT3\tWatts\n");
  LogRecord rec;
  for (uint32_t addr = 0; addr < currentFlashAddr; addr += sizeof(LogRecord)) {
    if (flash.readAnything(addr, rec)) {
      String l = String(addr) + "\t" + String(rec.timestamp) + "\t" +
                 String(rec.temp1, 1) + "\t" + String(rec.temp2, 1) + "\t" +
                 String(rec.temp3, 1) + "\t" + String(rec.pW, 1) + "\n";
      server.sendContent(l);
    }
    if (addr > 10000)
      break;
  }
  server.sendContent("--- END ---");
  server.sendContent("");
}

void handleDashboard() {
  sendHeader("Dashboard");
  server.sendContent("<div style='display:flex; align-items:center; gap:10px; "
                     "margin-bottom:15px;'>");
  server.sendContent("<h2 style='margin:0;'>📊 Telemetry</h2>");
  server.sendContent(
      "<select id='range' class='range-picker' onchange='loadHistory()'>");
  server.sendContent("<option value='0'>Live (10m)</option>");
  server.sendContent("<option value='1'>Last 1h</option>");
  server.sendContent("<option value='6'>Last 6h</option>");
  server.sendContent("<option value='12'>Last 12h</option>");
  server.sendContent("<option value='24'>Last 24h</option>");
  server.sendContent("<option value='48'>Last 48h</option>");
  server.sendContent("</select></div>");
  String grid = "<div class='grid'>";
  grid += "<div class='card t1'><div class='lab' id='l_t1'>" + s1Alias +
          "</div><div class='val' id='v_t1'>" +
          String(isfinite(t1) ? t1 : -127.0, 1) + "°C</div></div>";
  grid += "<div class='card t2'><div class='lab' id='l_t2'>" + s2Alias +
          "</div><div class='val' id='v_t2'>" +
          String(isfinite(t2) ? t2 : -127.0, 1) + "°C</div></div>";
  grid += "<div class='card t3'><div class='lab' id='l_t3'>" + s3Alias +
          "</div><div class='val' id='v_t3'>" +
          String(isfinite(t3) ? t3 : -127.0, 1) + "°C</div></div>";
  grid += "<div class='card'><div class='lab'>Main Power</div><div class='val' "
          "id='v_p'>" +
          String(isfinite(pWatts) ? pWatts : 0.0, 1) + "W</div>";
  grid += "<div class='lab' id='v_vi'>" +
          String(isfinite(vBus) ? vBus : 0.0, 1) + "V | " +
          String(isfinite(iAmps) ? iAmps : 0.0, 2) + "A</div></div></div>";
  server.sendContent(grid);
  server.sendContent("<div class='card' style='height:400px;'><canvas "
                     "id='myChart'></canvas></div>");
  server.sendContent(
      "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
  server.sendContent(
      "<script>if(typeof Chart === 'undefined') { alert('No internet'); } "
      "const ctx=document.getElementById('myChart');");
  server.sendContent(
      "let chart=new Chart(ctx,{type:'line',data:{labels:[],datasets:[");
  server.sendContent(
      "{label:'" + s1Alias +
      "',borderColor:'#32d1ff',backgroundColor:'rgba(50,209,255,0.1)',data:[],"
      "tension:0.4,pointRadius:0,borderWidth:1.5,fill:true},");
  server.sendContent(
      "{label:'" + s2Alias +
      "',borderColor:'#ff9d0a',backgroundColor:'rgba(255,157,10,0.1)',data:[],"
      "tension:0.4,pointRadius:0,borderWidth:1.5,fill:true},");
  server.sendContent(
      "{label:'" + s3Alias +
      "',borderColor:'#f85149',backgroundColor:'rgba(248,81,73,0.1)',data:[],"
      "tension:0.4,pointRadius:0,borderWidth:1.5,fill:true}]},");
  server.sendContent(
      "options:{responsive:true,maintainAspectRatio:false,interaction:{"
      "intersect:false,mode:'index'},plugins:{legend:{display:true,labels:{"
      "color:'#8b949e',boxWidth:12,usePointStyle:true}},tooltip:{"
      "backgroundColor:'#161b22',borderColor:'#30363d',borderWidth:1,padding:"
      "10,cornerRadius:4}},scales:{y:{grid:{color:'rgba(48,54,61,0.5)'},ticks:{"
      "color:'#8b949e'}},x:{grid:{display:false},ticks:{color:'#8b949e'}}}}})"
      ";");
  server.sendContent(
      "function update(){fetch('/api/data').then(r=>r.json()).then(d=>{");
  server.sendContent("document.getElementById('v_t1').innerText=d.t1+'°C';"
                     "document.getElementById('v_t2').innerText=d.t2+'°C';"
                     "document.getElementById('v_t3').innerText=d.t3+'°C';");
  server.sendContent(
      "document.getElementById('v_p').innerText=d.pWatts+'W';document."
      "getElementById('v_vi').innerText=d.vBus+'V | '+d.iAmps+'A';");
  server.sendContent(
      "if(chart.data.labels.length>120){chart.data.labels.shift()"
      ";chart.data.datasets.forEach(s=>s.data.shift());}");
  server.sendContent("chart.data.labels.push(d.time);chart.data.datasets[0]."
                     "data.push(d.t1);chart.data.datasets[1].data.push(d.t2);"
                     "chart.data.datasets[2].data.push(d.t3);");
  server.sendContent("chart.update();}).catch(e=>console.error(e));} ");
  server.sendContent(
      "function loadHistory(){ const h=document.getElementById('range').value; "
      "fetch('/api/history?h='+h).then(r=>r.json()).then(data=>{ "
      "chart.data.labels=[]; chart.data.datasets.forEach(s=>s.data=[]); "
      "data.forEach(d=>{ let dt=new Date(d.t*1000); let "
      "ts=dt.getHours()+':'+dt.getMinutes().toString().padStart(2,'0'); "
      "chart.data.labels.push(ts); "
      "chart.data.datasets[0].data.push(d.t1); "
      "chart.data.datasets[1].data.push(d.t2); "
      "chart.data.datasets[2].data.push(d.t3); }); chart.update(); }); } "
      "loadHistory(); setInterval(update, 5000); </script>");
  sendFooter();
}

void handleHardware() {
  sendHeader("Sensors");
  DeviceAddress da;
  String id1 = "---", id2 = "---", id3 = "---";
  if (sensors.getAddress(da, 0))
    id1 = getAddressStr(da);
  if (sensors.getAddress(da, 1))
    id2 = getAddressStr(da);
  if (sensors.getAddress(da, 2))
    id3 = getAddressStr(da);
  server.sendContent(
      "<h2>🛠 Hardware Inventory</h2><div "
      "class='card'><table><thead><tr><th>Device</th><th>Type</th><th>ID / "
      "Data</th><th>Config</th></tr></thead><tbody>");
  server.sendContent("<tr><td>Sensor Amb</td><td>DS18B20</td><td>ID: " + id1 +
                     "</td><td>OneWire (RX)</td></tr>");
  server.sendContent("<tr><td>Sensor Evap</td><td>DS18B20</td><td>ID: " + id2 +
                     "</td><td>OneWire (RX)</td></tr>");
  server.sendContent("<tr><td>Sensor Cond</td><td>DS18B20</td><td>ID: " + id3 +
                     "</td><td>OneWire (RX)</td></tr>");
  server.sendContent("<tr><td>Power Mon</td><td>INA219</td><td>I2C "
                     "0x40</td><td>D1, D2</td></tr>");
  server.sendContent("<tr><td>Memory</td><td>W25X80</td><td>MID: " +
                     String(manID, HEX) + "</td><td>SPI</td></tr>");
  server.sendContent("<tr><td>Display</td><td>SSD1306</td><td>128x32 "
                     "px</td><td>I2C 0x3C</td></tr>");
  server.sendContent(
      "<tr><td>WiFi</td><td>ESP8266</td><td>IP: " + WiFi.localIP().toString() +
      "</td><td>RSSI: " + String(WiFi.RSSI()) + "dBm</td></tr>");
  server.sendContent("</tbody></table></div>");
  sendFooter();
}

void handleDiag() {
  sendHeader("Diag");
  server.sendContent("<h2>📋 Diagnostics</h2>");
  server.sendContent(
      "<div class='grid'><div class='card'><h3>Memory</h3><p>Addr: " +
      String(currentFlashAddr) + "</p>");
  server.sendContent("<a href='/dump' class='btn btn-primary'>DUMP</a> <a "
                     "href='/wipe' class='btn btn-danger'>WIPE</a></div>");
  server.sendContent("<div class='card'><h3>Logs</h3><a href='/download' "
                     "class='btn btn-primary'>CSV EXPORT</a></div>");
  server.sendContent(
      "<div class='card'><h3>Cloud</h3><p>Res: " + String(lastTSResponse) +
      "</p><p>Sync: " + lastTSUploadTime + "</p></div></div>");
  sendFooter();
}

void handleSave() {
  if (server.hasArg("tml"))
    tMuestreoLocal = server.arg("tml").toInt();
  if (server.hasArg("tmc"))
    tMuestreoCloud = server.arg("tmc").toInt();
  if (server.hasArg("tsid"))
    tsChannelID = server.arg("tsid");
  if (server.hasArg("tskey"))
    tsAPIKey = server.arg("tskey");
  if (server.hasArg("s1a"))
    s1Alias = server.arg("s1a");
  if (server.hasArg("s2a"))
    s2Alias = server.arg("s2a");
  if (server.hasArg("s3a"))
    s3Alias = server.arg("s3a");
  saveSettings();
  if (DEBUG_MODE)
    Serial.println("Settings Saved to EEPROM");
  monitoreoActivo = true;
  server.sendHeader("Location", "/config");
  server.send(302, "text/plain", "");
}

// --- SETUP ---
void setup() {
  if (DEBUG_MODE) {
    delay(2000);
    Serial.begin(115200);
    Serial.println("\n--- OMNIDATA START ---");
  }
  u8g2.begin();
  u8g2.setFont(u8g2_font_5x7_tf);
  EEPROM.begin(512);
  loadSettings();
  for (int i = 0; i < 128; i++)
    historialT1[i] = 31;
  WiFiManager wm;
  if (!wm.autoConnect("Monitor-Taller-AP"))
    ESP.restart();
  timeClient.begin();
  sensors.begin();
  if (ina219.begin()) {
    if (DEBUG_MODE)
      Serial.println("INA OK");
  }
  if (flash.begin()) {
    manID = flash.getManID();
    if (DEBUG_MODE)
      Serial.println("Flash OK. MID: " + String(manID, HEX) + " Scanning...");
    currentFlashAddr = 0;
    while (currentFlashAddr < 1000000) {
      LogRecord tr;
      flash.readAnything(currentFlashAddr, tr);
      if (tr.timestamp == 0xFFFFFFFF)
        break;
      currentFlashAddr += sizeof(LogRecord);
      yield();
    }
    if (DEBUG_MODE)
      Serial.println("Scan Done. Addr: " + String(currentFlashAddr));
  }
  pinMode(ROTARY_ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENCODER_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ROTARY_ENCODER_A_PIN), handleEncoder,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_ENCODER_B_PIN), handleEncoder,
                  CHANGE);
  server.on("/", handleDashboard);
  server.on("/config", handleConfig);
  server.on("/hardware", handleHardware);
  server.on("/diag", handleDiag);
  server.on("/api/data", handleData);
  server.on("/api/history", handleHistory);
  server.on("/download", handleDownload);
  server.on("/wipe", handleWipe);
  server.on("/dump", handleDump);
  server.on("/save", handleSave);
  server.begin();
  tiempoInicio = millis();
}

void loop() {
  server.handleClient();
  timeClient.update();
  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 10000) {
    if (DEBUG_MODE)
      Serial.println("Heartbeat: IP " + WiFi.localIP().toString() + " OK");
    lastHeartbeat = millis();
  }
  static int lastTicks = 0;
  if (encoderTicks != lastTicks) {
    if (encoderTicks > lastTicks)
      sondaGraficada++;
    else
      sondaGraficada--;
    if (sondaGraficada > 2)
      sondaGraficada = 0;
    if (sondaGraficada < 0)
      sondaGraficada = 2;
    lastTicks = encoderTicks;
  }
  if (!monitoreoActivo) {
    uint32_t t = millis() - tiempoInicio;
    if (t >= WAIT_TIME) {
      monitoreoActivo = true;
    } else {
      static uint8_t qrb[176];
      static QRCode qr;
      static bool qrg = false;
      if (!qrg) {
        String u = "http://" + WiFi.localIP().toString();
        qrcode_initText(&qr, qrb, 1, 0, u.c_str());
        qrg = true;
      }
      u8g2.firstPage();
      do {
        // High contrast: White bg, black modules
        u8g2.setDrawColor(1);
        u8g2.drawBox(2, 2, 27, 27); // Quiet zone
        for (uint8_t y = 0; y < qr.size; y++) {
          for (uint8_t x = 0; x < qr.size; x++) {
            if (qrcode_getModule(&qr, x, y)) {
              u8g2.setDrawColor(0);
              u8g2.drawBox(x + 5, y + 5, 1, 1);
            }
          }
        }
        u8g2.setDrawColor(1);
        u8g2.setCursor(35, 12);
        u8g2.print("SCAN TO OPEN");
        u8g2.setCursor(35, 20);
        u8g2.print(WiFi.localIP().toString());
        u8g2.drawFrame(35, 24, 85, 5);
        u8g2.drawBox(37, 26, map(t, 0, WAIT_TIME, 0, 81), 1);
      } while (u8g2.nextPage());
      return;
    }
  }
  static uint32_t lastSample = 0;
  if (millis() - lastSample >= (tMuestreoLocal * 1000)) {
    flashTemp = true;
    sensors.requestTemperatures();
    t1 = sensors.getTempCByIndex(0);
    t2 = sensors.getTempCByIndex(1);
    t3 = sensors.getTempCByIndex(2);
    vBus = ina219.getBusVoltage_V();
    iAmps = ina219.getCurrent_mA() / 1000.0;
    pWatts = vBus * iAmps;
    for (int i = 0; i < 127; i++) {
      historialT1[i] = historialT1[i + 1];
      historialT2[i] = historialT2[i + 1];
      historialT3[i] = historialT3[i + 1];
    }
    historialT1[127] = (int)(t1 * 10);
    historialT2[127] = (int)(t2 * 10);
    historialT3[127] = (int)(t3 * 10);
    int *h_sel = (sondaGraficada == 0)
                     ? historialT1
                     : (sondaGraficada == 1 ? historialT2 : historialT3);
    float curMin = 999.0, curMax = -999.0;
    for (int i = 0; i < 128; i++) {
      float val = (float)h_sel[i] / 10.0;
      if (val < curMin)
        curMin = val;
      if (val > curMax)
        curMax = val;
    }
    yMin = curMin - 1.0;
    yMax = curMax + 1.0;
    if (yMax - yMin < 5.0) {
      yMax += 2.5;
      yMin -= 2.5;
    }
    LogRecord lr = {(uint32_t)timeClient.getEpochTime(), t1, t2, t3, pWatts};
    if (flash.writeAnything(currentFlashAddr, lr)) {
      currentFlashAddr += sizeof(LogRecord);
      flashError = 0;
    } else {
      flashError = (uint8_t)flash.error();
    }
    lastSample = millis();
  } else if (millis() - lastSample > 600) {
    flashTemp = false;
  }
  if (millis() - lastCloudUpload >= (tMuestreoCloud * 1000)) {
    if (WiFi.status() == WL_CONNECTED && tsAPIKey != "" &&
        tsAPIKey != "API_KEY") {
      WiFiClient client;
      HTTPClient http;
      String url = "http://api.thingspeak.com/update?api_key=" + tsAPIKey +
                   "&field1=" + String(t1, 1) + "&field2=" + String(t2, 1) +
                   "&field3=" + String(t3, 1);
      http.begin(client, url);
      lastTSResponse = http.GET();
      if (lastTSResponse == 200)
        lastTSUploadTime = timeClient.getFormattedTime();
      http.end();
    }
    lastCloudUpload = millis();
  }
  u8g2.firstPage();
  do {
    String hstr = timeClient.getFormattedTime().substring(0, 5);
    if (millis() % 1000 < 500)
      hstr.replace(":", " ");
    u8g2.setCursor(0, 7);
    u8g2.print(hstr);
    if (flashTemp) {
      u8g2.drawBox(30, 0, 98, 9);
      u8g2.setDrawColor(0);
      u8g2.setCursor(45, 7);
      u8g2.print("LECTURA OK");
      u8g2.setDrawColor(1);
    } else {
      u8g2.setCursor(32, 7);
      u8g2.print(sondaGraficada == 0 ? ">T1:" : " T1:");
      u8g2.print(t1, 0);
      u8g2.print(sondaGraficada == 1 ? " >T2:" : " T2:");
      u8g2.print(t2, 0);
      u8g2.print(sondaGraficada == 2 ? " >T3:" : " T3:");
      u8g2.print(t3, 0);
    }
    u8g2.drawHLine(0, 9, 128);
    u8g2.drawVLine(2, 10, 22);
    u8g2.drawHLine(2, 31, 126);
    int *h_d = (sondaGraficada == 0)
                   ? historialT1
                   : (sondaGraficada == 1 ? historialT2 : historialT3);
    for (int i = 0; i < 127; i++) {
      int y1 = map(h_d[i], (int)(yMin * 10), (int)(yMax * 10), 31, 11);
      int y2 = map(h_d[i + 1], (int)(yMin * 10), (int)(yMax * 10), 31, 11);
      u8g2.drawLine(i, constrain(y1, 11, 31), i + 1, constrain(y2, 11, 31));
    }
  } while (u8g2.nextPage());
}
