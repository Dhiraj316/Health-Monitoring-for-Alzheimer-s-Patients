/*
  Health Monitoring + Fall Detection + Medicine Reminder
  Board  : ESP32 Dev Module (Dual-Core)
  Sensors: MAX30100 (HR/SpO2), ADXL345 (Accel), DHT11 (Temp/Humidity), NEO-6M (GPS)
  Output : Active buzzer on GPIO 26, Password-Locked WiFi Web Dashboard
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <TinyGPS++.h>
#include <DHT.h>
#include "MAX30100_PulseOximeter.h"

// ===================== USER SETTINGS =====================
const char* WIFI_SSID    = "smile";
const char* WIFI_PASS    = "999999999";
const char* AP_SSID      = "AlzCare-Band";   // Fallback hotspot if home WiFi fails
const char* AP_PASS      = "12345678";       // Min 8 chars
const char* TZ_INFO      = "IST-5:30";       // India Standard Time

// Single-Patient Details & Web Lock Credentials
const char* PATIENT_NAME = "Ramesh Sharma (Ward A-101)";
const char* DASH_USER    = "admin";
const char* DASH_PASS    = "alz1234";

// Hardware Pins
#define PIN_SDA           21
#define PIN_SCL           22
#define PIN_GPS_RX        16   // ESP32 RX2 <- GPS TX
#define PIN_GPS_TX        17   // ESP32 TX2 -> GPS RX
#define PIN_DHT           4
#define PIN_BUZZER        26   // Direct connection on GPIO 26
#define DHTTYPE           DHT11
#define BUZZER_ACTIVE_LOW 0    // Set 1 for low-level-trigger buzzer modules

// Fall detection tuning (units: g)
#define FREEFALL_G        0.5f
#define IMPACT_G          2.5f
#define SETTLE_MS         1000   // Ignore bounce right after impact
#define WINDOW_MS         2000   // Must stay still this long to confirm
#define INACT_RANGE_G     0.30f  // max-min of |a| allowed while "still"
#define REQUIRE_FREEFALL  0      // 1 = require a free-fall phase before impact
#define MED_TIMEOUT_MS    120000UL
// =========================================================

#define ADXL_ADDR  0x53
#define MAX_ALARMS 6

struct Alarm {
  uint8_t hour;
  uint8_t minute;
  uint8_t enabled;
  char label[16];
};

WebServer server(80);
Preferences prefs;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);
DHT dht(PIN_DHT, DHTTYPE);
PulseOximeter pox;

Alarm alarms[MAX_ALARMS];

// Sensor states
bool maxOk = false, adxlOk = false, dhtOk = false, staMode = false;
float hrVal = 0, spo2Val = 0;
bool hrValid = false;
float tempC = 0, humV = 0;
uint8_t dhtFail = 0;
float ax = 0, ay = 0, az = 0, mag = 1.0f;

// Alerts & Counters
bool medActive = false, fallActive = false;
unsigned long medStart = 0;
char medLabel[16] = "Medicine";
char lastEvent[48] = "None yet";
uint16_t takenCount = 0, missedCount = 0;
double fallLat = 0, fallLon = 0;
bool fallHasPos = false;
char fallAt[12] = "--:--:--";

// Fall State Machine
enum { FS_IDLE, FS_SETTLE, FS_WINDOW };
uint8_t fallsafe = FS_IDLE;
unsigned long fsT = 0, ffT = 0;
float wMin = 1, wMax = 1;

unsigned long lastAcc = 0, lastPox = 0, lastDht = 0, lastSec = 0;
int lastMinuteKey = -1;

// ---------------------------------------------------------
// Embedded Web Interface with Security Lock & Fall Siren
// ---------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AlzCare Band - Patient Monitor</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--tx:#f8fafc;--mut:#94a3b8;--ok:#10b981;--bad:#ef4444;--warn:#f59e0b;--acc:#3b82f6;--bd:#334155}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--tx);padding:14px;max-width:1100px;margin:auto}
header{display:flex;justify-content:space-between;align-items:center;background:var(--card);padding:14px 18px;border-radius:12px;border:1px solid var(--bd);margin-bottom:14px;flex-wrap:wrap;gap:10px}
h1{font-size:19px;margin:0;display:flex;align-items:center;gap:8px}
h2{font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--bad)}
.dot.ok{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px}
.big{font-size:36px;font-weight:700;line-height:1.1}
.big small{font-size:14px;color:var(--mut);font-weight:400}
.sub{color:var(--mut);font-size:12.5px;margin-top:4px}
.row{display:flex;justify-content:space-between;padding:4px 0;font-size:14px}
.row b{font-variant-numeric:tabular-nums}
.banner{padding:14px 16px;border-radius:12px;margin-bottom:12px;color:#fff;display:flex;justify-content:space-between;align-items:center;gap:10px;font-weight:700;animation:pulse 1.2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.82}}
.banner.fall{background:var(--bad);border:2px solid #fca5a5}.banner.med{background:#d97706;border:2px solid #fde68a}
.banner button{background:#fff;color:#111;border:0;border-radius:8px;padding:8px 14px;font-weight:700}
button{cursor:pointer;border:1px solid var(--bd);background:#0f172a;color:var(--tx);border-radius:8px;padding:8px 12px;font-size:13.5px;font-weight:600}
button.p{background:var(--acc);color:#fff;border-color:var(--acc)}
.arow{display:grid;grid-template-columns:70px 115px 1fr 75px;gap:8px;align-items:center;padding:8px 0;border-top:1px solid var(--bd)}
.arow input[type=text],.arow input[type=time]{width:100%;padding:8px;border-radius:8px;border:1px solid var(--bd);background:#0f172a;color:var(--tx);font-size:14px}
.chips span{display:inline-block;font-size:11.5px;padding:3px 9px;border-radius:99px;margin:2px 4px 2px 0;background:var(--bd)}
.chips span.ok{background:rgba(16,185,129,.2);color:#34d399;border:1px solid #059669}
.chips span.bad{background:rgba(239,68,68,.2);color:#f87171;border:1px solid #b91c1c}
canvas{width:100%;height:60px;margin-top:6px}
a{color:#60a5fa}
#toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--acc);color:#fff;padding:9px 18px;border-radius:99px;opacity:0;transition:.3s;pointer-events:none;font-weight:600;z-index:999}
#toast.show{opacity:1}
#lockScreen{position:fixed;inset:0;background:rgba(15,23,42,.96);backdrop-filter:blur(8px);display:flex;align-items:center;justify-content:center;z-index:100;padding:16px}
.login-box{background:var(--card);border:1px solid var(--bd);border-radius:16px;padding:24px;width:100%;max-width:390px;box-shadow:0 20px 50px rgba(0,0,0,.5)}
.login-box input{width:100%;padding:10px 12px;margin:6px 0 14px;border-radius:8px;border:1px solid var(--bd);background:#0f172a;color:#fff;font-size:14px}
@media(max-width:520px){.arow{grid-template-columns:60px 1fr 65px}.arow input[type=text]{grid-column:1/3}}
</style></head><body>

<!-- SECURITY LOCK SCREEN -->
<div id="lockScreen">
  <div class="login-box">
    <h1 style="margin-bottom:6px">AlzCare Band Lock</h1>
    <div class="sub" style="margin-bottom:16px">Enter Caregiver Username & Password to view live patient telemetry and modify medicine buzzer alarms.</div>
    <label class="sub">Username</label>
    <input type="text" id="userIn" placeholder="Enter username" autocomplete="username">
    <label class="sub">Password</label>
    <input type="password" id="passIn" placeholder="Enter password" autocomplete="current-password">
    <div id="loginErr" style="color:var(--bad);font-size:13px;margin-bottom:10px;display:none">Invalid username or password!</div>
    <button class="p" style="width:100%;padding:11px" onclick="doLogin()">Unlock Patient Monitor</button>
  </div>
</div>

<header>
  <div>
    <h1><span id="conn" class="dot"></span> AlzCare Wearable Monitor</h1>
    <div class="sub" id="patHeader">Patient: --</div>
  </div>
  <div style="display:flex;align-items:center;gap:10px">
    <b id="clock" style="font-family:monospace;font-size:16px">--:--:--</b>
    <button onclick="doLogout()">Lock Screen</button>
  </div>
</header>

<div id="banners"></div>

<div class="grid">
 <section class="card"><h2>Heart rate (MAX30100)</h2>
  <div class="big"><span id="hr">--</span> <small>bpm</small></div>
  <div class="sub" id="hrnote">Place finger on sensor</div>
  <canvas id="spark" width="300" height="60"></canvas></section>

 <section class="card"><h2>Blood oxygen (SpO2)</h2>
  <div class="big"><span id="spo2">--</span> <small>%</small></div>
  <div class="sub">Needs 10-20 s of steady finger contact</div></section>

 <section class="card"><h2>Ambient Air (DHT11)</h2>
  <div class="row"><span>Temperature</span><b><span id="temp">--</span> &deg;C</b></div>
  <div class="row"><span>Humidity</span><b><span id="hum">--</span> %</b></div>
  <div class="sub">GPIO 4 | Room Environment</div></section>

 <section class="card"><h2>Motion / Fall (ADXL345)</h2>
  <div class="big" id="fallstate">--</div>
  <div class="row"><span>X / Y / Z (g)</span><b><span id="ax">--</span> / <span id="ay">--</span> / <span id="az">--</span></b></div>
  <div class="row"><span>Magnitude</span><b><span id="mag">--</span> g</b></div></section>

 <section class="card"><h2>GPS Location (NEO-6M)</h2>
  <div class="row"><span>Fix Status</span><b id="fix">--</b></div>
  <div class="row"><span>Latitude</span><b id="lat">--</b></div>
  <div class="row"><span>Longitude</span><b id="lon">--</b></div>
  <div class="row"><span>Satellites</span><b id="sat">--</b></div>
  <div class="row"><span>Speed / Alt</span><b><span id="spd">--</span> km/h | <span id="alt">--</span> m</b></div>
  <div class="sub" id="maplink"></div></section>

 <section class="card"><h2>Device & Buzzer (GPIO 26)</h2>
  <div class="chips" id="chips"></div>
  <div class="row"><span>Network IP</span><b id="net">--</b></div>
  <div class="row"><span>Uptime</span><b id="up">--</b></div>
  <div class="row"><span>Last Medicine Event</span><b id="last">--</b></div>
  <div class="row"><span>Taken / Missed</span><b id="cnt">--</b></div>
  <div style="margin-top:8px;display:flex;gap:6px;flex-wrap:wrap">
    <button onclick="syncTime()">Sync Clock</button>
    <button class="p" onclick="testBuzz()">Test GPIO 26 Buzzer</button>
  </div></section>
</div>

<section class="card" style="margin-top:12px">
  <h2>Medicine Buzzer Schedule (GPIO 26)</h2>
  <div class="sub" style="margin-bottom:6px">Set time, medicine name, tick On, and press Save. Stored permanently in ESP32 Flash memory.</div>
  <div id="alarms"></div>
</section>

<div id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const p2=n=>String(n).padStart(2,'0');
const f=(v,d)=>(v===null||v===undefined)?'--':Number(v).toFixed(d);
let hist=[],busy=false,synced=false,unlocked=false,authToken='';
let audioCtx=null;

function beepBrowser(freq,dur){
  try{
    if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();
    const o=audioCtx.createOscillator(),g=audioCtx.createGain();
    o.frequency.value=freq;o.type='sawtooth';
    o.connect(g);g.connect(audioCtx.destination);
    o.start();g.gain.exponentialRampToValueAtTime(0.0001,audioCtx.currentTime+dur);
    o.stop(audioCtx.currentTime+dur);
  }catch(e){}
}

function toast(t){const e=$('toast');e.textContent=t;e.className='show';setTimeout(()=>e.className='',2000)}
async function post(url,body){
  return fetch(url,{method:'POST',headers:{'X-Auth':authToken},body:body||''});
}

async function doLogin(){
  const u=$('userIn').value.trim(),p=$('passIn').value;
  const r=await fetch('/api/login',{method:'POST',body:new URLSearchParams({user:u,pass:p})});
  if(!r.ok){$('loginErr').style.display='block';return;}
  const d=await r.json();
  authToken=d.token;
  unlocked=true;
  $('loginErr').style.display='none';
  $('lockScreen').style.display='none';
  $('patHeader').textContent='Patient: '+d.patient;
  loadAlarms();poll();
  toast('Dashboard Unlocked');
}

function doLogout(){
  unlocked=false;authToken='';
  $('passIn').value='';
  $('lockScreen').style.display='flex';
}

async function syncTime(){const r=await post('/api/time',new URLSearchParams({epoch:Math.floor(Date.now()/1000)}));toast(r.ok?'Time synced':'Sync failed')}
async function testBuzz(){await post('/api/test');toast('GPIO 26 Buzzer test started')}
async function ackMed(){await post('/api/ack');poll()}
async function clearFall(){await post('/api/clearfall');poll()}

function spark(){
 const c=$('spark'),x=c.getContext('2d');x.clearRect(0,0,c.width,c.height);
 const v=hist.filter(a=>a!==null);if(v.length<2)return;
 const mn=Math.min(...v)-5,mx=Math.max(...v)+5;
 x.strokeStyle='#ef4444';x.lineWidth=2;x.beginPath();
 hist.forEach((a,i)=>{if(a===null)return;const px=i*c.width/59,py=c.height-(a-mn)/(mx-mn)*c.height;i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke();
}

function render(d){
 $('clock').textContent=d.time;
 $('patHeader').textContent='Patient: '+d.patient;
 $('hr').textContent=d.hr===null?'--':d.hr;
 $('hrnote').textContent=d.finger?'Reading OK':'Place finger on sensor';
 $('spo2').textContent=d.spo2===null?'--':d.spo2;
 $('temp').textContent=f(d.temp,1);$('hum').textContent=f(d.hum,0);
 $('ax').textContent=f(d.ax,2);$('ay').textContent=f(d.ay,2);$('az').textContent=f(d.az,2);$('mag').textContent=f(d.mag,2);
 $('fallstate').textContent=['Normal','Checking Impact...','FALL DETECTED!'][d.fall];
 $('fallstate').style.color=d.fall==2?'var(--bad)':(d.fall==1?'var(--warn)':'var(--ok)');
 $('fix').textContent=d.fix?'Locked':'Searching...';
 $('lat').textContent=f(d.lat,6);$('lon').textContent=f(d.lon,6);
 $('sat').textContent=d.sat;$('spd').textContent=f(d.spd,1);$('alt').textContent=f(d.alt,0);
 $('maplink').innerHTML=d.lat!==null?'<a target="_blank" href="https://www.google.com/maps?q='+d.lat+','+d.lon+'">Open in Google Maps &rarr;</a>'+(d.fix?'':' (last known)'):'Waiting for satellite lock...';
 $('net').textContent=d.mode+' ('+d.ip+')';
 const u=d.up;$('up').textContent=Math.floor(u/3600)+'h '+Math.floor(u%3600/60)+'m '+(u%60)+'s';
 $('last').textContent=d.last;$('cnt').textContent=d.taken+' / '+d.missed;
 $('chips').innerHTML=[['MAX30100',d.ok.max],['ADXL345',d.ok.adxl],['DHT11',d.ok.dht],['GPS UART2',d.ok.gps],['Clock',d.timeOk]]
  .map(a=>'<span class="'+(a[1]?'ok':'bad')+'">'+a[0]+'</span>').join('');
 let b='';
 if(d.fall==2){
   b+='<div class="banner fall"><span>EMERGENCY: FALL DETECTED at '+d.fallAt+(d.fallLat!==null?' - <a style="color:#fff;text-decoration:underline" target="_blank" href="https://www.google.com/maps?q='+d.fallLat+','+d.fallLon+'">GPS Map Location</a>':'')+'</span><button onclick="clearFall()">Acknowledge & Clear</button></div>';
   beepBrowser(880,0.25);
 }
 if(d.med.active){
   b+='<div class="banner med"><span>MEDICINE TIME: '+d.med.label+' [GPIO 26 Buzzer Active]</span><button onclick="ackMed()">Taken / Stop Buzzer</button></div>';
   beepBrowser(520,0.15);
 }
 $('banners').innerHTML=b;
 hist.push(d.hr);if(hist.length>60)hist.shift();spark();
 if(!d.timeOk&&!synced){synced=true;syncTime()}
}

async function poll(){
 if(!unlocked||busy)return;busy=true;
 try{const c=new AbortController();const t=setTimeout(()=>c.abort(),2500);
  const r=await fetch('/api/data',{cache:'no-store',headers:{'X-Auth':authToken},signal:c.signal});clearTimeout(t);
  if(r.status===401){doLogout();busy=false;return;}
  render(await r.json());$('conn').className='dot ok';
 }catch(e){$('conn').className='dot'}
 busy=false;
}

async function loadAlarms(){
 if(!unlocked)return;
 const a=await(await fetch('/api/alarms',{headers:{'X-Auth':authToken}})).json();const box=$('alarms');box.innerHTML='';
 a.forEach(x=>{const r=document.createElement('div');r.className='arow';
  r.innerHTML='<label><input type="checkbox" id="en'+x.i+'" '+(x.en?'checked':'')+'> On</label>'+
  '<input type="time" id="tm'+x.i+'" value="'+p2(x.h)+':'+p2(x.m)+'">'+
  '<input type="text" id="lb'+x.i+'" maxlength="15" value="'+x.label+'" placeholder="Medicine name">'+
  '<button class="p" onclick="saveAlarm('+x.i+')">Save</button>';box.appendChild(r)});
}

async function saveAlarm(i){
 const v=$('tm'+i).value;if(!v){toast('Pick a time');return}
 const [h,m]=v.split(':');
 const r=await post('/api/alarm',new URLSearchParams({i:i,h:parseInt(h),m:parseInt(m),en:$('en'+i).checked?1:0,label:$('lb'+i).value}));
 toast(r.ok?'Saved to ESP32 Flash':'Error saving');
}

$('passIn').addEventListener('keydown',e=>{if(e.key==='Enter')doLogin()});
setInterval(poll,1000);
</script></body></html>
)rawliteral";

// ---------------------------------------------------------
// Helpers & Flash Storage
// ---------------------------------------------------------
bool isAuthorized() {
  String expected = String(DASH_USER) + ":" + String(DASH_PASS);
  return server.header("X-Auth") == expected;
}

bool timeValid() { return time(nullptr) > 1700000000; }

void timeString(char* buf, size_t n, const char* fmt) {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(buf, n, fmt, &tmv);
}

void buzzerWrite(bool on) {
  digitalWrite(PIN_BUZZER, (on ^ (BUZZER_ACTIVE_LOW ? 1 : 0)) ? HIGH : LOW);
}

void setEvent(const char* what) {
  char tb[12] = "--:--";
  if (timeValid()) timeString(tb, sizeof(tb), "%H:%M");
  snprintf(lastEvent, sizeof(lastEvent), "%s %s %s", medLabel, what, tb);
}

void startMed(const char* label) {
  if (medActive) return;
  strncpy(medLabel, (label && label[0]) ? label : "Medicine", sizeof(medLabel) - 1);
  medLabel[sizeof(medLabel) - 1] = 0;
  medActive = true;
  medStart = millis();
  Serial.printf("[MED] Reminder: %s\n", medLabel);
}

void loadAlarms() {
  if (prefs.getBytesLength("alarms") == sizeof(alarms)) {
    prefs.getBytes("alarms", alarms, sizeof(alarms));
  } else {
    memset(alarms, 0, sizeof(alarms));
    alarms[0] = {8, 0, 1, "Morning Dose"};
    alarms[1] = {14, 0, 1, "Afternoon"};
    alarms[2] = {20, 0, 1, "Night Dose"};
  }
  for (int i = 0; i < MAX_ALARMS; i++) alarms[i].label[15] = 0;
}

void saveAlarms() { prefs.putBytes("alarms", alarms, sizeof(alarms)); }

// ---------------------------------------------------------
// ADXL345 (Raw I2C, no library)
// ---------------------------------------------------------
bool adxlWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool adxlRead(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool adxlInit() {
  uint8_t id = 0;
  if (!adxlRead(0x00, &id, 1) || id != 0xE5) return false;
  adxlWrite(0x2C, 0x0A);  // 100 Hz
  adxlWrite(0x31, 0x0B);  // full resolution, +/-16g
  adxlWrite(0x2D, 0x08);  // measure mode
  return true;
}

void readAccel() {
  uint8_t b[6];
  if (!adxlRead(0x32, b, 6)) return;
  ax = (int16_t)((b[1] << 8) | b[0]) * 0.0039f;
  ay = (int16_t)((b[3] << 8) | b[2]) * 0.0039f;
  az = (int16_t)((b[5] << 8) | b[4]) * 0.0039f;
  mag = sqrtf(ax * ax + ay * ay + az * az);
}

// ---------------------------------------------------------
// Fall detection: (optional free-fall) -> impact -> settle -> stillness check
// ---------------------------------------------------------
void fallUpdate(float m) {
  unsigned long now = millis();
  switch (fallsafe) {
    case FS_IDLE:
      if (m < FREEFALL_G) ffT = now;
      if (m > IMPACT_G) {
        if (REQUIRE_FREEFALL && !(ffT && (now - ffT) < 800)) break;
        fallsafe = FS_SETTLE;
        fsT = now;
        Serial.println("[FALL] Impact seen, checking stillness...");
      }
      break;
    case FS_SETTLE:
      if (now - fsT >= SETTLE_MS) {
        fallsafe = FS_WINDOW;
        fsT = now;
        wMin = wMax = m;
      }
      break;
    case FS_WINDOW:
      if (m < wMin) wMin = m;
      if (m > wMax) wMax = m;
      if (now - fsT >= WINDOW_MS) {
        if ((wMax - wMin) < INACT_RANGE_G) {
          fallActive = true;
          fallHasPos = gps.location.isValid();
          fallLat = gps.location.lat();
          fallLon = gps.location.lng();
          if (timeValid()) timeString(fallAt, sizeof(fallAt), "%H:%M:%S");
          Serial.println("[FALL] CONFIRMED!");
        } else {
          Serial.println("[FALL] Movement after impact - false alarm cancelled.");
        }
        fallsafe = FS_IDLE;
        ffT = 0;
      }
      break;
  }
}

// ---------------------------------------------------------
// Buzzer patterns on GPIO 26 (non-blocking). Fall overrides medicine.
// ---------------------------------------------------------
void buzzerUpdate() {
  bool on = false;
  unsigned long now = millis();
  if (fallActive) {
    on = ((now / 150) % 2) == 0;                       // Fast continuous emergency beeping
  } else if (medActive) {
    unsigned long ph = (now - medStart) % 1600;        // Double-beep ... pause pattern
    on = (ph < 200) || (ph >= 400 && ph < 600);
  }
  buzzerWrite(on);
}

// ---------------------------------------------------------
// Medicine alarm check (once per second)
// ---------------------------------------------------------
void checkAlarms() {
  if (medActive && (millis() - medStart > MED_TIMEOUT_MS)) {
    medActive = false;
    missedCount++;
    setEvent("MISSED");
  }
  if (!timeValid()) return;
  time_t n = time(nullptr);
  struct tm t;
  localtime_r(&n, &t);
  int key = t.tm_hour * 60 + t.tm_min;
  if (key == lastMinuteKey) return;
  lastMinuteKey = key;

  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].enabled && alarms[i].hour == t.tm_hour && alarms[i].minute == t.tm_min) {
      startMed(alarms[i].label);
      break;
    }
  }
}

// ---------------------------------------------------------
// JSON helpers + HTTP handlers
// ---------------------------------------------------------
void jnum(String& s, const char* k, double v, int dec, bool valid) {
  s += '"'; s += k; s += "\":";
  if (valid && !isnan(v)) s += String(v, dec); else s += "null";
  s += ',';
}
void jbool(String& s, const char* k, bool v) { s += '"'; s += k; s += "\":"; s += (v ? "true" : "false"); s += ','; }
void jstr(String& s, const char* k, const String& v) { s += '"'; s += k; s += "\":\""; s += v; s += "\","; }

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleLogin() {
  if (server.arg("user") == DASH_USER && server.arg("pass") == DASH_PASS) {
    String token = String(DASH_USER) + ":" + String(DASH_PASS);
    String res = "{\"token\":\"" + token + "\",\"patient\":\"" + String(PATIENT_NAME) + "\"}";
    server.send(200, "application/json", res);
  } else {
    server.send(401, "text/plain", "Unauthorized");
  }
}

void handleData() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  char tb[12] = "--:--:--";
  if (timeValid()) timeString(tb, sizeof(tb), "%H:%M:%S");
  bool fix = gps.location.isValid() && gps.location.age() < 10000;
  int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  String s;
  s.reserve(1150);
  s += '{';
  jstr(s, "patient", PATIENT_NAME);
  jnum(s, "hr", hrVal, 0, hrValid);
  jnum(s, "spo2", spo2Val, 0, hrValid && spo2Val > 0);
  jnum(s, "temp", tempC, 1, dhtOk);
  jnum(s, "hum", humV, 0, dhtOk);
  jnum(s, "ax", ax, 2, adxlOk);
  jnum(s, "ay", ay, 2, adxlOk);
  jnum(s, "az", az, 2, adxlOk);
  jnum(s, "mag", mag, 2, adxlOk);
  jnum(s, "lat", gps.location.lat(), 6, gps.location.isValid());
  jnum(s, "lon", gps.location.lng(), 6, gps.location.isValid());
  jnum(s, "spd", gps.speed.kmph(), 1, gps.speed.isValid());
  jnum(s, "alt", gps.altitude.meters(), 0, gps.altitude.isValid());
  jbool(s, "fix", fix);
  s += "\"sat\":"; s += sats; s += ',';
  jbool(s, "finger", hrValid);
  s += "\"fall\":"; s += (fallActive ? 2 : (fallsafe != FS_IDLE ? 1 : 0)); s += ',';
  jstr(s, "fallAt", fallAt);
  jnum(s, "fallLat", fallLat, 6, fallActive && fallHasPos);
  jnum(s, "fallLon", fallLon, 6, fallActive && fallHasPos);
  s += "\"med\":{\"active\":"; s += (medActive ? "true" : "false");
  s += ",\"label\":\""; s += medLabel; s += "\"},";
  jstr(s, "time", tb);
  jbool(s, "timeOk", timeValid());
  jstr(s, "mode", staMode ? "WiFi" : "Hotspot");
  jstr(s, "ip", staMode ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  jstr(s, "last", lastEvent);
  s += "\"taken\":"; s += takenCount; s += ',';
  s += "\"missed\":"; s += missedCount; s += ',';
  s += "\"up\":"; s += (unsigned long)(millis() / 1000); s += ',';
  s += "\"ok\":{\"max\":"; s += (maxOk ? "true" : "false");
  s += ",\"adxl\":"; s += (adxlOk ? "true" : "false");
  s += ",\"dht\":"; s += (dhtOk ? "true" : "false");
  s += ",\"gps\":"; s += (gps.charsProcessed() > 20 ? "true" : "false");
  s += "}}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleAlarmsGet() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String s = "[";
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (i) s += ',';
    s += "{\"i\":"; s += i;
    s += ",\"h\":"; s += alarms[i].hour;
    s += ",\"m\":"; s += alarms[i].minute;
    s += ",\"en\":"; s += alarms[i].enabled;
    s += ",\"label\":\""; s += alarms[i].label; s += "\"}";
  }
  s += ']';
  server.send(200, "application/json", s);
}

void handleAlarmSet() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!server.hasArg("i") || !server.hasArg("h") || !server.hasArg("m")) {
    server.send(400, "text/plain", "missing");
    return;
  }
  int i = server.arg("i").toInt(), h = server.arg("h").toInt(), m = server.arg("m").toInt();
  if (i < 0 || i >= MAX_ALARMS || h < 0 || h > 23 || m < 0 || m > 59) {
    server.send(400, "text/plain", "range");
    return;
  }
  alarms[i].hour = h;
  alarms[i].minute = m;
  alarms[i].enabled = (server.arg("en") == "1");
  String lb = server.arg("label");
  int k = 0;
  for (size_t j = 0; j < lb.length() && k < 15; j++) {
    unsigned char c = (unsigned char)lb[j];
    if (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') alarms[i].label[k++] = c;
  }
  alarms[i].label[k] = 0;
  saveAlarms();
  lastMinuteKey = -1;
  server.send(200, "text/plain", "ok");
}

void handleAck() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (medActive) { medActive = false; takenCount++; setEvent("taken"); }
  server.send(200, "text/plain", "ok");
}

void handleClearFall() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  fallActive = false;
  fallsafe = FS_IDLE;
  server.send(200, "text/plain", "ok");
}

void handleTest() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  startMed("Buzzer Test");
  server.send(200, "text/plain", "ok");
}

void handleTime() {
  if (!isAuthorized()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!server.hasArg("epoch")) { server.send(400, "text/plain", "missing"); return; }
  unsigned long e = strtoul(server.arg("epoch").c_str(), NULL, 10);
  if (e < 1700000000UL) { server.send(400, "text/plain", "bad"); return; }
  struct timeval tv = {(time_t)e, 0};
  settimeofday(&tv, NULL);
  lastMinuteKey = -1;
  server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------
void i2cScan() {
  Serial.println("I2C scan (expect 0x53 ADXL345, 0x57 MAX30100):");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("  found 0x%02X\n", a); found++; }
  }
  if (!found) Serial.println("  nothing found - check SDA/SCL wiring and 3.3V");
}

// Background task on Core 0 for slow DHT11 & HTTP Server so Core 1 MAX30100 never drops out
void slowPeripheralsTask(void *pvParameters) {
  for (;;) {
    unsigned long now = millis();
    if (now - lastDht >= 2500) {
      lastDht = now;
      float t = dht.readTemperature(), h = dht.readHumidity();
      if (!isnan(t) && !isnan(h)) { tempC = t; humV = h; dhtOk = true; dhtFail = 0; }
      else if (++dhtFail >= 3) dhtOk = false;
    }
    if (now - lastSec >= 1000) {
      lastSec = now;
      checkAlarms();
    }
    server.handleClient();
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
  Serial.println("\n=== AlzCare Single-Patient Locked Band ===");

  prefs.begin("alz_single", false);
  loadAlarms();

  Wire.begin(PIN_SDA, PIN_SCL);
  i2cScan();

  adxlOk = adxlInit();
  Serial.printf("ADXL345 : %s\n", adxlOk ? "OK" : "NOT FOUND");

  maxOk = pox.begin();
  if (maxOk) pox.setIRLedCurrent(MAX30100_LED_CURR_27_1MA);
  Serial.printf("MAX30100: %s\n", maxOk ? "OK" : "NOT FOUND");

  dht.begin();
  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // WiFi: try home network, else start own hotspot
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print('.'); }
  if (WiFi.status() == WL_CONNECTED) {
    staMode = true;
    Serial.printf("\nWiFi OK. Open http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("\nNo WiFi. Hotspot '%s' pass '%s' -> http://%s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  }
  WiFi.setSleep(false);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  if (MDNS.begin("alzcare")) Serial.println("Also try http://alzcare.local");

  // Tell WebServer to collect the custom X-Auth header for lock verification
  const char* headerKeys[] = {"X-Auth"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/alarms", HTTP_GET, handleAlarmsGet);
  server.on("/api/alarm", HTTP_POST, handleAlarmSet);
  server.on("/api/ack", HTTP_POST, handleAck);
  server.on("/api/clearfall", HTTP_POST, handleClearFall);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/time", HTTP_POST, handleTime);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  // Start WebServer & DHT11 on Core 0
  xTaskCreatePinnedToCore(slowPeripheralsTask, "SlowTask", 8192, NULL, 1, NULL, 0);

  // Short startup chirp on GPIO 26
  buzzerWrite(true); delay(120); buzzerWrite(false);
}

void loop() {
  unsigned long now = millis();

  // Runs uninterrupted on Core 1 so MAX30100 FIFO and Fall Detection never stall
  if (maxOk) pox.update();
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  if (adxlOk && now - lastAcc >= 10) {            // 100 Hz Fall Detection State Machine
    lastAcc = now;
    readAccel();
    fallUpdate(mag);
  }

  if (maxOk && now - lastPox >= 1000) {           // 1 Hz HR/SpO2 snapshot
    lastPox = now;
    float h = pox.getHeartRate();
    float o = pox.getSpO2();
    hrValid = (h >= 40 && h <= 200);
    hrVal = hrValid ? h : 0;
    spo2Val = (hrValid && o >= 50 && o <= 100) ? o : 0;
  }

  buzzerUpdate();
}
