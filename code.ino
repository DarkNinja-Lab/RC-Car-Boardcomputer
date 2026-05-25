#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>

#define PIN_ESC    12
#define PIN_STEER  14
#define PIN_SW     13
#define PIN_HEAD    2
#define PIN_TAIL    4
#define PIN_BLINK   5

#define MID_L  1400
#define MID_H  1600
#define RIGHT  1800
#define LEFT   1200
#define RPM_MAX  22000

#define DRL       80
#define HEAD_F   255
#define TAIL_D    60
#define TAIL_F   255
#define BLINK_T    400
#define SEQ_T       50
#define FLASH_T    150
#define DEBOUNCE   100

enum Mode { M_OFF, M_DRL, M_HEAD, M_HAZARD, M_LEFT, M_RIGHT };
enum ThrottleState { TH_NEUTRAL, TH_GAS, TH_BRAKE, TH_REVERSE };

struct Chan {
  byte pin;
  volatile unsigned long us;
  volatile unsigned long start;
  volatile unsigned long lastMs;
};

Chan rc[3] = {{PIN_ESC, 1500, 0, 0}, {PIN_STEER, 1500, 0, 0}, {PIN_SW, 1500, 0, 0}};
Mode mode = M_OFF;
int cycle = 0, prevPos = 0;
bool blinkOn = false, emergency = false;
unsigned long brakeT = 0, blinkT = 0, lastSw = 0, seqT = 0;
int bHead = 0, bTail = 0, bBlink = 0, seqStep = 0;
ThrottleState thState = TH_NEUTRAL;
bool reverseReady = false;

const float VOLT_MULTIPLIER = 0.0110;
float battVolt = 0.0;
int battPercent = 0;

const char* ssid = "RC-Buggy";
const char* pass = "12345678";
ESP8266WebServer srv(80);
WebSocketsServer webSocket = WebSocketsServer(81);

void IRAM_ATTR isr0() {
  if (digitalRead(rc[0].pin)) rc[0].start = micros();
  else { rc[0].us = micros() - rc[0].start; rc[0].lastMs = millis(); }
}
void IRAM_ATTR isr1() {
  if (digitalRead(rc[1].pin)) rc[1].start = micros();
  else { rc[1].us = micros() - rc[1].start; rc[1].lastMs = millis(); }
}
void IRAM_ATTR isr2() {
  if (digitalRead(rc[2].pin)) rc[2].start = micros();
  else { rc[2].us = micros() - rc[2].start; rc[2].lastMs = millis(); }
}

void handleRoot();
void handleManifest();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);

const char MANIFEST[] PROGMEM = R"rawliteral({"name":"RC Dash","short_name":"Buggy","start_url":"/","display":"standalone","background_color":"#050505","theme_color":"#ff0040","orientation":"portrait"})rawliteral";

const char HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<meta name="theme-color" content="#ff0040">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<link rel="manifest" href="/manifest.json">
<title>1:8 Reely Rex-X (Buggy)</title>
<style>
:root{
  --bg:#050505;
  -- dial:#121212;
  --ring:rgba(255,255,255,0.08);
  --text:#e8e8e8;
  --muted:#555;
  --accent:#ff0040;
  --cyan:#00d4ff;
  --orange:#ff6b00;
  --purple:#b829dd;
  --green:#00ff88;
  --yellow:#ffcc00;
  --glass: rgba(255,255,255,0.03);
}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
html,body{height:100%;overflow:hidden;background:var(--bg);color:var(--text);font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif;}

/* Carbon-Textur Hintergrund */
body::before{
  content:'';position:fixed;inset:0;
  background:
    radial-gradient(ellipse at 50% 100%, rgba(255,0,64,0.08) 0%, transparent 50%),
    radial-gradient(ellipse at 0% 0%, rgba(0,212,255,0.05) 0%, transparent 40%),
    repeating-linear-gradient(90deg, rgba(255,255,255,0.01) 0px, rgba(255,255,255,0.01) 1px, transparent 1px, transparent 4px),
    repeating-linear-gradient(0deg, rgba(255,255,255,0.01) 0px, rgba(255,255,255,0.01) 1px, transparent 1px, transparent 4px);
  pointer-events:none;z-index:0;
}

.container{position:relative;z-index:1;height:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px;gap:14px;}

/* === HEADER === */
.header{width:100%;max-width:480px;display:flex;justify-content:space-between;align-items:center;}
.brand{font-size:0.75rem;letter-spacing:6px;color:var(--accent);text-transform:uppercase;font-weight:800;text-shadow:0 0 20px rgba(255,0,64,0.4);}

.battery{display:flex;align-items:center;gap:8px;background:var(--glass);border:1px solid var(--ring);border-radius:10px;padding:6px 12px;backdrop-filter:blur(10px);}
.batt-icon{width:24px;height:11px;border:1.5px solid rgba(255,255,255,0.25);border-radius:3px;position:relative;padding:2px;}
.batt-icon::after{content:'';position:absolute;right:-4px;top:2.5px;width:3px;height:6px;background:rgba(255,255,255,0.25);border-radius:0 1px 1px 0;}
.batt-fill{height:100%;width:0%;background:var(--green);border-radius:1px;transition:width 0.5s ease,background 0.3s;box-shadow:0 0 8px rgba(0,255,136,0.3);}
.batt-text{display:flex;flex-direction:column;font-size:0.6rem;line-height:1.3;}
.batt-v{color:#fff;font-weight:700;font-family:'SF Mono',monospace;font-size:0.7rem;}
.batt-p{color:var(--muted);font-size:0.55rem;}

/* === HAUPT TACHO === */
.gauge-cluster{position:relative;width:min(92vw, 420px);height:min(92vw, 420px);display:flex;align-items:center;justify-content:center;}

/* Äußerer Ring mit Lichtschein */
.gauge-outer{
  position:absolute;width:100%;height:100%;border-radius:50%;
  background:conic-gradient(from 225deg, rgba(255,0,64,0.15) 0deg, rgba(255,0,64,0.05) 45deg, transparent 90deg, transparent 270deg, rgba(0,212,255,0.05) 315deg, rgba(255,0,64,0.15) 360deg);
  box-shadow:
    0 0 60px rgba(255,0,64,0.1),
    inset 0 0 40px rgba(0,0,0,0.8),
    0 0 0 2px rgba(255,255,255,0.06);
  filter:blur(0.5px);
}
.gauge-rim{
  position:absolute;width:96%;height:96%;border-radius:50%;
  border:3px solid rgba(255,255,255,0.06);
  box-shadow:
    inset 0 0 20px rgba(0,0,0,0.9),
    0 0 0 8px rgba(10,10,10,0.8),
    0 0 0 9px rgba(255,255,255,0.04);
}

/* Zifferblatt-Hintergrund */
.gauge-face{
  position:absolute;width:88%;height:88%;border-radius:50%;
  background:
    radial-gradient(circle at 50% 50%, #1a1a1a 0%, #0a0a0a 70%, #050505 100%);
  box-shadow:inset 0 0 30px rgba(0,0,0,1), 0 0 20px rgba(0,0,0,0.5);
}

/* Tick Container */
.ticks{position:absolute;width:82%;height:82%;border-radius:50%;pointer-events:none;}
.tick{
  position:absolute;top:50%;left:50%;width:2px;background:rgba(255,255,255,0.2);
  transform-origin:center bottom;
  transform:translate(-50%, -100%) rotate(var(--a)) translateY(-130px);
  height:var(--h);
}
.tick.major{background:rgba(255,255,255,0.6);width:3px;height:18px;}
.tick.red{background:var(--accent);box-shadow:0 0 8px rgba(255,0,64,0.6);width:3px;height:20px;}
.tick-label{
  position:absolute;top:50%;left:50%;
  transform:translate(-50%,-50%) rotate(var(--a)) translateY(-105px) rotate(calc(var(--a)*-1));
  font-size:0.85rem;font-weight:700;color:rgba(255,255,255,0.7);font-family:'SF Mono',monospace;
}
.tick-label.red{color:var(--accent);text-shadow:0 0 10px rgba(255,0,64,0.5);}

/* Glare-Effekt (Glas-Reflexion) */
.glass-glare{
  position:absolute;width:88%;height:88%;border-radius:50%;
  background:linear-gradient(135deg, rgba(255,255,255,0.08) 0%, transparent 40%, transparent 60%, rgba(255,255,255,0.02) 100%);
  pointer-events:none;z-index:20;
}

/* Nadel */
.needle-hub{
  position:absolute;bottom:50%;left:50%;width:4px;height:42%;z-index:15;
  transform-origin:bottom center;transform:translateX(-50%) rotate(-90deg);
  will-change:transform;transition:none;
}
.needle{
  position:absolute;top:0;left:50%;transform:translateX(-50%);
  width:0;height:0;
  border-left:3px solid transparent;border-right:3px solid transparent;border-bottom:170px solid var(--accent);
  filter:drop-shadow(0 0 6px rgba(255,0,64,0.8)) drop-shadow(0 0 2px rgba(255,255,255,0.3));
}
.needle::after{
  content:'';position:absolute;top:0;left:50%;transform:translateX(-50%);
  width:0;height:0;border-left:1.5px solid transparent;border-right:1.5px solid transparent;border-bottom:170px solid rgba(255,255,255,0.3);
}
.pivot{
  position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
  width:24px;height:24px;border-radius:50%;background:#1a1a1a;z-index:16;
  border:2px solid rgba(255,255,255,0.15);
  box-shadow:0 0 15px rgba(0,0,0,0.8), inset 0 2px 4px rgba(255,255,255,0.1);
}
.pivot::after{
  content:'';position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
  width:8px;height:8px;border-radius:50%;background:var(--accent);
  box-shadow:0 0 12px rgba(255,0,64,0.8);
}

/* Digitale Anzeige im Tacho */
.digital-rpm{
  position:absolute;bottom:28%;left:50%;transform:translateX(-50%);text-align:center;z-index:12;
}
.digital-rpm .num{
  font-size:3.2rem;font-weight:800;font-family:'SF Mono','Courier New',monospace;color:#fff;
  line-height:1;text-shadow:0 0 25px rgba(255,255,255,0.15), 0 2px 4px rgba(0,0,0,0.8);
  letter-spacing:-3px;
}
.digital-rpm .unit{font-size:0.6rem;color:var(--muted);letter-spacing:3px;text-transform:uppercase;margin-top:2px;font-weight:600;}
.rpm-bar{
  position:absolute;bottom:22%;left:50%;transform:translateX(-50%);width:120px;height:3px;background:rgba(255,255,255,0.05);border-radius:2px;overflow:hidden;
}
.rpm-bar-fill{height:100%;width:0%;background:linear-gradient(90deg,var(--accent),var(--orange));border-radius:2px;transition:width 0.1s linear;box-shadow:0 0 10px rgba(255,0,64,0.3);}

/* === UNTERE ANZEIGEN === */
.info-grid{width:100%;max-width:480px;display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;}
.info-cell{
  background:var(--glass);border:1px solid var(--ring);border-radius:12px;padding:12px;text-align:center;
  backdrop-filter:blur(10px);position:relative;overflow:hidden;
  box-shadow:0 4px 20px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.04);
}
.info-cell::before{
  content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,transparent,rgba(255,255,255,0.1),transparent);
}
.info-cell .label{font-size:0.55rem;color:var(--muted);letter-spacing:2px;text-transform:uppercase;margin-bottom:6px;font-weight:600;}
.info-cell .value{font-size:0.95rem;font-weight:700;color:#fff;}
.info-cell.active{border-color:rgba(255,0,64,0.3);background:rgba(255,0,64,0.04);box-shadow:0 0 20px rgba(255,0,64,0.08), inset 0 1px 0 rgba(255,255,255,0.05);}
.info-cell.active .value{color:var(--accent);text-shadow:0 0 12px rgba(255,0,64,0.3);}
.info-cell.brake{border-color:rgba(255,0,0,0.4);background:rgba(255,0,0,0.05);animation:brakePulse 0.6s ease-in-out infinite alternate;}
.info-cell.brake .value{color:#ff3333;text-shadow:0 0 15px rgba(255,0,0,0.5);}
.info-cell.rev{border-color:rgba(184,41,221,0.3);background:rgba(184,41,221,0.04);}
.info-cell.rev .value{color:var(--purple);text-shadow:0 0 12px rgba(184,41,221,0.3);}
@keyframes brakePulse{from{box-shadow:0 0 15px rgba(255,0,0,0.1);}to{box-shadow:0 0 30px rgba(255,0,0,0.25);}}

/* === BALKEN (Lenkung & Gas) === */
.bar-section{width:100%;max-width:480px;display:flex;flex-direction:column;gap:10px;}
.bar-row{display:flex;align-items:center;gap:12px;background:var(--glass);border:1px solid var(--ring);border-radius:10px;padding:10px 14px;}
.bar-name{width:70px;font-size:0.6rem;color:var(--muted);text-transform:uppercase;letter-spacing:1.5px;font-weight:700;}
.bar-track{flex:1;height:8px;background:rgba(255,255,255,0.04);border-radius:4px;overflow:hidden;position:relative;}
.bar-fill{height:100%;width:0%;border-radius:4px;transition:width 0.08s linear,background 0.2s;position:relative;}
.bar-fill::after{content:'';position:absolute;right:0;top:0;bottom:0;width:15px;background:linear-gradient(90deg,transparent,rgba(255,255,255,0.25));border-radius:0 4px 4px 0;}
.fill-steer{background:linear-gradient(90deg,var(--cyan),#80f0ff);box-shadow:0 0 12px rgba(0,212,255,0.15);}
.fill-gas{background:linear-gradient(90deg,var(--orange),#ffaa55);box-shadow:0 0 12px rgba(255,107,0,0.15);}
.fill-brake{background:linear-gradient(90deg,#ff0000,var(--accent));box-shadow:0 0 15px rgba(255,0,64,0.25);}
.fill-rev{background:linear-gradient(90deg,var(--purple),#e066ff);box-shadow:0 0 15px rgba(184,41,221,0.2);}
.bar-num{width:45px;text-align:right;font-family:'SF Mono',monospace;font-size:0.75rem;color:#fff;font-weight:700;}

/* === SPARKLINE === */
.spark-box{width:100%;max-width:480px;height:50px;background:var(--glass);border:1px solid var(--ring);border-radius:10px;overflow:hidden;position:relative;}
.spark-box canvas{width:100%;height:100%;display:block;}

/* === STATUS ZEILE === */
.status-bar{width:100%;max-width:480px;display:flex;justify-content:space-between;align-items:center;font-family:'SF Mono',monospace;font-size:0.55rem;color:#333;padding:0 4px;}
.ws-badge{font-size:0.55rem;color:var(--muted);background:rgba(0,0,0,0.4);padding:3px 8px;border-radius:4px;border:1px solid rgba(255,255,255,0.05);}

/* Rotations-Animation für Warnung */
@keyframes spin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}
</style>
</head>
<body>
<div class="container">

  <div class="header">
    <div class="brand">Dynora</div>
    <div class="battery" id="battBox">
      <div class="batt-icon"><div class="batt-fill" id="battLevel"></div></div>
      <div class="batt-text">
        <span class="batt-v" id="battV">--.-V</span>
        <span class="batt-p" id="battP">--%</span>
      </div>
    </div>
  </div>

  <div class="gauge-cluster" id="gaugeCluster">
    <div class="gauge-outer"></div>
    <div class="gauge-rim"></div>
    <div class="gauge-face"></div>
    <div class="ticks" id="ticks"></div>
    <div class="glass-glare"></div>
    
    <div class="needle-hub" id="needleHub"><div class="needle"></div></div>
    <div class="pivot"></div>
    
    <div class="digital-rpm">
      <div class="num" id="rpmNum">0</div>
      <div class="unit">x1000 U/min</div>
    </div>
    <div class="rpm-bar"><div class="rpm-bar-fill" id="rpmBar"></div></div>
  </div>

  <div class="info-grid">
    <div class="info-cell" id="cMode">
      <div class="label">Licht</div>
      <div class="value" id="tMode">AUS</div>
    </div>
    <div class="info-cell" id="cThrot">
      <div class="label">Antrieb</div>
      <div class="value" id="tThrot">NEUTRAL</div>
    </div>
    <div class="info-cell" id="cBatt">
      <div class="label">Akku</div>
      <div class="value" id="tBatt">--%</div>
    </div>
  </div>

  <div class="bar-section">
    <div class="bar-row">
      <div class="bar-name">Lenkung</div>
      <div class="bar-track"><div class="bar-fill fill-steer" id="barSteer"></div></div>
      <div class="bar-num" id="valSteer">1500</div>
    </div>
    <div class="bar-row">
      <div class="bar-name" id="lblThrot">Gas</div>
      <div class="bar-track"><div class="bar-fill fill-gas" id="barThrot"></div></div>
      <div class="bar-num" id="valThrot">1500</div>
    </div>
  </div>

  <div class="spark-box"><canvas id="spark" width="480" height="50"></canvas></div>

  <div class="status-bar">
    <span id="rawSteer">ST: 1500</span>
    <span id="rawThrot">TH: 1500</span>
    <span class="ws-badge" id="wsStatus">WS: ...</span>
  </div>

</div>

<script>
const MAX_RPM=22000;
const ticksContainer=document.getElementById('ticks');

// Tacho-Skala generieren (halber Kreis, 225° bis -45° / also 270° Spanne, aber wir zeigen nur 180° oben)
// Wir machen einen klassischen 180° Tacho: -90° (links) bis +90° (rechts)
for(let i=0;i<=MAX_RPM;i+=1000){
  const angle=-90+(i/MAX_RPM)*180;
  const isMajor=i%5000===0;
  const isRed=i>=20000;
  const tick=document.createElement('div');
  tick.className='tick'+(isMajor?' major':'')+(isRed?' red':'');
  tick.style.setProperty('--a',angle+'deg');
  tick.style.setProperty('--h',isMajor?(isRed?'20px':'18px'):'10px');
  ticksContainer.appendChild(tick);
  
  if(isMajor){
    const lbl=document.createElement('div');
    lbl.className='tick-label'+(isRed?' red':'');
    lbl.style.setProperty('--a',angle+'deg');
    lbl.textContent=(i/1000);
    ticksContainer.appendChild(lbl);
  }
}

const canvas=document.getElementById('spark');
const ctx=canvas.getContext('2d');
const hist=new Array(120).fill(0);

let target={rpm:0,steer:1500,throt:1500,th:0,thn:'NEUTRAL',m:'AUS',v:0,p:0};
let current={rpm:0,steer:1500,throt:1500};
let ws;

function lerp(a,b,t){return a+(b-a)*t;}

function connect(){
  const host=location.hostname||'192.168.4.1';
  ws=new WebSocket('ws://'+host+':81');
  ws.onopen=()=>{
    document.getElementById('wsStatus').innerText='WS: live';
    document.getElementById('wsStatus').style.color='var(--green)';
  };
  ws.onmessage=(e)=>{
    let d=JSON.parse(e.data);
    target.rpm=d.r;target.steer=d.s;target.throt=d.t;
    target.th=d.th;target.thn=d.thn;target.m=d.m;
    target.v=d.v;target.p=d.p;
    hist.push(d.r);hist.shift();
  };
  ws.onclose=()=>{
    document.getElementById('wsStatus').innerText='WS: retry';
    document.getElementById('wsStatus').style.color='var(--orange)';
    setTimeout(connect,1500);
  };
}
connect();

function drawSpark(){
  const w=canvas.width,h=canvas.height;
  ctx.clearRect(0,0,w,h);
  const grad=ctx.createLinearGradient(0,0,0,h);
  grad.addColorStop(0,'rgba(255,0,64,0.2)');
  grad.addColorStop(1,'rgba(255,0,64,0)');
  ctx.beginPath();ctx.moveTo(0,h);
  for(let i=0;i<hist.length;i++){
    let x=i*(w/(hist.length-1));
    let y=h-(hist[i]/MAX_RPM)*h*0.9;
    ctx.lineTo(x,y);
  }
  ctx.lineTo(w,h);ctx.fillStyle=grad;ctx.fill();
  ctx.beginPath();
  for(let i=0;i<hist.length;i++){
    let x=i*(w/(hist.length-1));
    let y=h-(hist[i]/MAX_RPM)*h*0.9;
    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
  }
  ctx.strokeStyle='var(--accent)';ctx.lineWidth=1.5;ctx.stroke();
}

function updateUI(){
  // Weiche Übergänge
  current.rpm=lerp(current.rpm,target.rpm,0.15);
  current.steer=lerp(current.steer,target.steer,0.2);
  current.throt=lerp(current.throt,target.throt,0.2);

  // Nadelrotation (-90 bis +90 Grad)
  const angle=-90+Math.min(current.rpm/MAX_RPM,1)*180;
  document.getElementById('needleHub').style.transform=`translateX(-50%) rotate(${angle}deg)`;
  
  // Digitale RPM
  document.getElementById('rpmNum').innerText=Math.round(current.rpm);
  document.getElementById('rpmBar').style.width=Math.min((current.rpm/MAX_RPM)*100,100)+'%';

  // Lenkungs-Balken (zentriert)
  const sDev=current.steer-1500;
  const sPct=Math.min(Math.abs(sDev)/5,50);
  const sBar=document.getElementById('barSteer');
  sBar.style.left=sDev<<0?(50-sPct)+'%':'50%';
  sBar.style.width=sPct+'%';
  document.getElementById('valSteer').innerText=Math.round(current.steer);

  // Gas/Bremse/Rückwärts Balken
  const tDev=current.throt-1500;
  const tPct=Math.min(Math.abs(tDev)/5,50);
  const tBar=document.getElementById('barThrot');
  const tLbl=document.getElementById('lblThrot');
  tBar.style.left=tDev<<0?(50-tPct)+'%':'50%';
  tBar.style.width=tPct+'%';
  
  if(target.th===1){tBar.className='bar-fill fill-gas';tLbl.innerText='Gas';}
  else if(target.th===2){tBar.className='bar-fill fill-brake';tLbl.innerText='Bremse';}
  else if(target.th===3){tBar.className='bar-fill fill-rev';tLbl.innerText='Rückwärts';}
  else{tBar.className='bar-fill';tLbl.innerText='Neutral';tBar.style.width='0%';tBar.style.left='50%';}
  document.getElementById('valThrot').innerText=Math.round(current.throt);

  // Info-Zellen Styling
  const cMode=document.getElementById('cMode');
  cMode.className='info-cell'+(target.m!=='AUS'?' active':'');
  document.getElementById('tMode').innerText=target.m;

  const cThrot=document.getElementById('cThrot');
  cThrot.className='info-cell';
  if(target.th===2)cThrot.classList.add('brake');
  else if(target.th===3)cThrot.classList.add('rev');
  document.getElementById('tThrot').innerText=target.thn;

  // Akku
  document.getElementById('tBatt').innerText=target.p+'%';
  document.getElementById('battV').innerText=target.v.toFixed(1)+'V';
  document.getElementById('battP').innerText=target.p+'%';
  const bl=document.getElementById('battLevel');
  bl.style.width=Math.max(0,Math.min(100,target.p))+'%';
  bl.style.background=target.p>50?'var(--green)':(target.p>20?'var(--yellow)':'#ff0000');

  // Raw-Werte
  document.getElementById('rawSteer').innerText='ST: '+Math.round(current.steer);
  document.getElementById('rawThrot').innerText='TH: '+Math.round(current.throt);

  drawSpark();
  requestAnimationFrame(updateUI);
}
updateUI();
</script>
</body>
</html>
)rawliteral";

void handleRoot() { srv.send(200, "text/html", HTML); }
void handleManifest() { srv.send(200, "application/json", MANIFEST); }
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {}

void boot() {
  for (int i = 0; i <= 255; i += 60) {
    analogWrite(PIN_HEAD, i); analogWrite(PIN_TAIL, i); analogWrite(PIN_BLINK, i);
    delay(4);
  }
  delay(40);
  for (int i = 255; i >= 0; i -= 60) {
    analogWrite(PIN_HEAD, i); analogWrite(PIN_TAIL, i); analogWrite(PIN_BLINK, i);
    delay(2);
  }
}

void updateBlinker() {
  unsigned long now = millis();
  if (now - blinkT >= BLINK_T) {
    blinkT = now; blinkOn = !blinkOn; seqStep = 0; seqT = now;
    if (!blinkOn) { analogWrite(PIN_BLINK, 0); bBlink = 0; }
  }
  if (!blinkOn) return;
  if (seqStep < 3 && now - seqT >= SEQ_T) {
    seqT = now;
    int b = map(seqStep, 0, 2, 85, 255);
    analogWrite(PIN_BLINK, b); bBlink = b;
    seqStep++;
  }
}

int getPos(long v) {
  if (v < 900 || v > 2500) return 0;
  if (v >= MID_L && v <= MID_H) return 2;
  if (v >= RIGHT) return 3;
  if (v <= LEFT) return 1;
  return 0;
}

void updateThrottle(long g) {
  bool inGas = g > 1550;
  bool inNeutral = (g >= 1450 && g <= 1550);
  bool inLow = g < 1450;
  if (inGas) { thState = TH_GAS; reverseReady = false; }
  else if (inNeutral) { if (thState == TH_BRAKE) reverseReady = true; thState = TH_NEUTRAL; }
  else if (inLow) { if (thState == TH_NEUTRAL || thState == TH_GAS) thState = reverseReady ? TH_REVERSE : TH_BRAKE; }
}

void updateLights() {
  if (mode == M_OFF || mode == M_HAZARD) return;
  static long lastG = 1500;
  if (thState == TH_BRAKE && (lastG - rc[0].us) > 300 && !emergency) { emergency = true; brakeT = millis(); }
  if (thState != TH_BRAKE) emergency = false;
  lastG = rc[0].us;
  if (thState == TH_BRAKE) {
    int b = TAIL_F;
    if (emergency) { unsigned long e = millis() - brakeT; if (e < FLASH_T * 6) b = ((e / FLASH_T) % 2) ? TAIL_D : TAIL_F; else emergency = false; }
    if (bTail != b) { analogWrite(PIN_TAIL, b); bTail = b; }
  } else if (bTail != TAIL_D) { analogWrite(PIN_TAIL, TAIL_D); bTail = TAIL_D; }
}

void updateBattery() {
  static unsigned long lastBatt = 0;
  if (millis() - lastBatt > 500) {
    lastBatt = millis();
    int raw = analogRead(A0);
    battVolt = raw * VOLT_MULTIPLIER;
    battPercent = map(constrain(battVolt, 9.9, 12.6), 9.9, 12.6, 0, 100);
  }
}

void setMode(Mode m) {
  if (m == mode) return;
  mode = m;
  switch (m) {
    case M_OFF: analogWrite(PIN_HEAD, 0); bHead = 0; analogWrite(PIN_TAIL, 0); bTail = 0; analogWrite(PIN_BLINK, 0); bBlink = 0; break;
    case M_DRL: analogWrite(PIN_HEAD, DRL); bHead = DRL; analogWrite(PIN_TAIL, TAIL_D); bTail = TAIL_D; analogWrite(PIN_BLINK, 0); bBlink = 0; break;
    case M_HEAD: analogWrite(PIN_HEAD, HEAD_F); bHead = HEAD_F; analogWrite(PIN_TAIL, TAIL_D); bTail = TAIL_D; analogWrite(PIN_BLINK, 0); bBlink = 0; break;
    case M_HAZARD: analogWrite(PIN_HEAD, 0); bHead = 0; analogWrite(PIN_TAIL, 0); bTail = 0; blinkT = millis(); blinkOn = false; seqStep = 0; break;
    case M_LEFT: case M_RIGHT: blinkT = millis(); blinkOn = false; seqStep = 0; break;
  }
}

String mName() {
  switch (mode) { case M_OFF: return "AUS"; case M_DRL: return "DRL"; case M_HEAD: return "ABBLEND"; case M_HAZARD: return "WARNBLINKER"; case M_LEFT: return "BLINKER L"; case M_RIGHT: return "BLINKER R"; }
  return "-";
}
String thName() {
  switch (thState) { case TH_NEUTRAL: return "NEUTRAL"; case TH_GAS: return "GAS"; case TH_BRAKE: return "BREMSE"; case TH_REVERSE: return "RÜCKWÄRTS"; }
  return "-";
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_HEAD, OUTPUT); pinMode(PIN_TAIL, OUTPUT); pinMode(PIN_BLINK, OUTPUT);
  analogWrite(PIN_HEAD, 0); analogWrite(PIN_TAIL, 0); analogWrite(PIN_BLINK, 0);
  pinMode(PIN_ESC, INPUT); pinMode(PIN_STEER, INPUT); pinMode(PIN_SW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ESC), isr0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_STEER), isr1, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_SW), isr2, CHANGE);
  delay(300);
  boot();
  setMode(M_OFF);
  WiFi.softAP(ssid, pass);
  MDNS.begin("buggy");
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  srv.on("/", handleRoot);
  srv.on("/manifest.json", handleManifest);
  srv.begin();
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("ws", "tcp", 81);
}

void loop() {
  webSocket.loop();
  srv.handleClient();
  unsigned long ms = millis();
  noInterrupts();
  long g = rc[0].us; long s = rc[1].us; long sw = rc[2].us;
  interrupts();
  if (ms - rc[0].lastMs > 100) g = 1500;
  if (ms - rc[1].lastMs > 100) s = 1500;
  if (ms - rc[2].lastMs > 100) sw = 1500;
  updateThrottle(g);
  updateLights();
  updateBattery();
  int pos = getPos(sw);
  if (pos && pos != prevPos && (ms - lastSw > DEBOUNCE)) {
    if (prevPos) {
      if (prevPos == 3 && pos == 2) { if (cycle == 0) { setMode(M_DRL); cycle = 1; } else if (cycle == 2) { setMode(M_HAZARD); cycle = 3; } }
      else if (prevPos == 2 && pos == 3) { if (cycle == 1) { setMode(M_HEAD); cycle = 2; } else if (cycle == 3) { setMode(M_OFF); cycle = 0; } }
      else if (pos == 1 && prevPos == 2) setMode(M_LEFT);
      else if (prevPos == 1 && pos == 2) setMode(cycle >= 2 ? M_HEAD : M_DRL);
    }
    prevPos = pos; lastSw = ms;
  }
  if (mode == M_HAZARD || mode == M_LEFT || mode == M_RIGHT) updateBlinker();
  static unsigned long lastWs = 0;
  if (ms - lastWs > 50) {
    lastWs = ms;
    int rpm = 0;
    if (thState == TH_GAS) rpm = map(constrain(g, 1500, 2000), 1500, 2000, 0, RPM_MAX);
    else if (thState == TH_REVERSE) rpm = map(constrain(g, 1000, 1500), 1500, 1000, 0, RPM_MAX / 2);
    String json = "{\"s\":" + String(s) + ",\"t\":" + String(g) + ",\"r\":" + String(rpm) + ",\"m\":\"" + mName() + "\",\"th\":" + String(thState) + ",\"thn\":\"" + thName() + "\",\"v\":" + String(battVolt, 1) + ",\"p\":" + battPercent + "}";
    webSocket.broadcastTXT(json);
  }
  delay(1);
}
