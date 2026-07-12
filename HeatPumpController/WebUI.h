#pragma once

// ============================================================
//  EMBEDDED DASHBOARD UI
//  Stored as a raw C++ string literal in firmware flash.
//  Served by WebServerManager on GET /
//
//  Design: Industrial SCADA dark-mode, card-based layout.
//  Cards: Device Status, Electrical, Temperature, Control, Logs
//  API:
//    On load: GET /api/info, GET /api/config
//    Polling: GET /api/live every 1000ms
// ============================================================

const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>HeatPump Controller</title>
<style>
  :root {
    --bg:        #0d0f14;
    --surface:   #161a23;
    --surface2:  #1e2330;
    --border:    #2a3045;
    --accent:    #00d4ff;
    --accent2:   #7c3aed;
    --green:     #22c55e;
    --yellow:    #f59e0b;
    --red:       #ef4444;
    --text:      #e2e8f0;
    --text-muted:#64748b;
    --font:      'Segoe UI', system-ui, sans-serif;
    --radius:    12px;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--font);
    min-height: 100vh;
    padding: 16px;
  }

  /* ---- Header ---- */
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 24px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    margin-bottom: 20px;
  }
  header h1 {
    font-size: 1.2rem;
    font-weight: 700;
    letter-spacing: 0.04em;
    color: var(--accent);
  }
  header .meta { font-size: 0.78rem; color: var(--text-muted); text-align: right; }
  header .meta span { display: block; }

  /* ---- Grid ---- */
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
    gap: 16px;
  }
  .grid-wide { grid-column: 1 / -1; }

  /* ---- Card ---- */
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 20px;
    position: relative;
    overflow: hidden;
    transition: border-color 0.3s;
  }
  .card:hover { border-color: var(--accent); }
  .card::before {
    content: '';
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 3px;
    background: linear-gradient(90deg, var(--accent), var(--accent2));
    opacity: 0.7;
  }
  .card-title {
    font-size: 0.72rem;
    font-weight: 600;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--text-muted);
    margin-bottom: 16px;
  }

  /* ---- Status indicator ---- */
  .status-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 0;
    border-bottom: 1px solid var(--border);
  }
  .status-row:last-child { border-bottom: none; }
  .status-label { font-size: 0.85rem; color: var(--text-muted); }
  .dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    display: inline-block;
    margin-right: 6px;
  }
  .dot-green  { background: var(--green);  box-shadow: 0 0 8px var(--green);  }
  .dot-red    { background: var(--red);    box-shadow: 0 0 8px var(--red);    }
  .dot-yellow { background: var(--yellow); box-shadow: 0 0 8px var(--yellow); }
  .dot-grey   { background: #444; }

  /* ---- Metric card ---- */
  .metric-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
    gap: 12px;
  }
  .metric {
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 12px;
    text-align: center;
    transition: transform 0.2s;
  }
  .metric:hover { transform: translateY(-2px); }
  .metric-label {
    font-size: 0.7rem;
    color: var(--text-muted);
    letter-spacing: 0.06em;
    text-transform: uppercase;
    margin-bottom: 6px;
  }
  .metric-value {
    font-size: 1.5rem;
    font-weight: 700;
    color: var(--accent);
    line-height: 1;
  }
  .metric-unit {
    font-size: 0.72rem;
    color: var(--text-muted);
    margin-top: 4px;
  }
  .metric-status {
    font-size: 0.65rem;
    margin-top: 6px;
    font-weight: 600;
    letter-spacing: 0.05em;
  }
  .online  { color: var(--green); }
  .offline { color: var(--red);   }

  /* ---- State badge ---- */
  .state-badge {
    display: inline-block;
    padding: 4px 12px;
    border-radius: 20px;
    font-size: 0.78rem;
    font-weight: 700;
    letter-spacing: 0.08em;
  }
  .state-MONITORING { background: #1e3a5f; color: var(--accent); }
  .state-AUTO_MODE  { background: #14532d; color: var(--green);  }
  .state-FAULT      { background: #450a0a; color: var(--red);    }
  .state-LOCKOUT    { background: #450a0a; color: var(--red);    }
  .state-BOOTING,
  .state-CONNECTING_WIFI,
  .state-SYNC_TIME  { background: #431407; color: var(--yellow); }
  .state-MANUAL_MODE{ background: #3b1f0f; color: var(--yellow); }
  .state-OTA_UPDATE { background: #2d1b69; color: #a78bfa;      }
  .state-default    { background: var(--surface2); color: var(--text-muted); }

  /* ---- Alarm pills ---- */
  .alarm-list { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 8px; }
  .alarm-pill {
    background: #450a0a;
    border: 1px solid var(--red);
    color: var(--red);
    padding: 4px 10px;
    border-radius: 20px;
    font-size: 0.72rem;
    font-weight: 600;
    letter-spacing: 0.06em;
    animation: pulse-red 1.5s infinite;
  }
  @keyframes pulse-red {
    0%,100% { opacity: 1; }
    50%      { opacity: 0.5; }
  }
  .no-alarms { color: var(--green); font-size: 0.85rem; }

  /* ---- Control buttons ---- */
  .btn-row { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 12px; }
  .btn {
    padding: 8px 20px;
    border-radius: 8px;
    border: 1px solid transparent;
    font-size: 0.82rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    letter-spacing: 0.04em;
  }
  .btn-primary   { background: var(--accent);  color: #000; }
  .btn-danger    { background: var(--red);     color: #fff; }
  .btn-warning   { background: var(--yellow);  color: #000; }
  .btn-secondary { background: var(--surface2); color: var(--text); border-color: var(--border); }
  .btn:hover     { opacity: 0.85; transform: translateY(-1px); }
  .btn:active    { transform: translateY(0); }

  /* ---- Logs ---- */
  .log-list {
    max-height: 220px;
    overflow-y: auto;
    font-family: 'Courier New', monospace;
    font-size: 0.75rem;
  }
  .log-entry {
    padding: 5px 0;
    border-bottom: 1px solid var(--border);
    display: flex;
    gap: 10px;
    line-height: 1.4;
  }
  .log-ts    { color: var(--text-muted); white-space: nowrap; }
  .log-INFO  { color: #60a5fa; }
  .log-WARN  { color: var(--yellow); }
  .log-ERROR { color: var(--red); }
  .log-CRIT  { color: #ff0000; font-weight: bold; }
  .log-msg   { color: var(--text); }

  /* ---- Divider ---- */
  hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }

  /* ---- Placeholder card accent ---- */
  .card-placeholder { opacity: 0.45; }
  .card-placeholder::before { background: var(--border); }

  /* ---- Pulse animation for live indicator ---- */
  @keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.3} }
  .live-dot {
    width: 8px; height: 8px;
    background: var(--green);
    border-radius: 50%;
    display: inline-block;
    margin-right: 6px;
    animation: blink 1s infinite;
  }

  /* ---- Water tank level bar ---- */
  .water-bar-wrap {
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 8px;
    height: 24px;
    overflow: hidden;
    margin: 10px 0;
    position: relative;
  }
  .water-bar-fill {
    height: 100%;
    border-radius: 8px;
    transition: width 0.8s ease, background 0.5s ease;
    background: linear-gradient(90deg, #0ea5e9, #06b6d4);
  }
  .water-bar-fill.low      { background: linear-gradient(90deg, #f59e0b, #fbbf24); }
  .water-bar-fill.critical { background: linear-gradient(90deg, #ef4444, #f87171); animation: pulse-red 1s infinite; }
  .water-bar-fill.empty    { background: #ef4444; }
  .water-bar-label {
    position: absolute;
    right: 10px; top: 50%;
    transform: translateY(-50%);
    font-size: 0.78rem;
    font-weight: 700;
    color: var(--text);
    text-shadow: 0 1px 4px #000;
  }
  .water-state-badge {
    display: inline-block;
    padding: 3px 10px;
    border-radius: 12px;
    font-size: 0.75rem;
    font-weight: 700;
    letter-spacing: 0.06em;
  }
  .ws-FULL     { background: #164e63; color: #67e8f9; }
  .ws-HIGH     { background: #1e3a5f; color: var(--accent); }
  .ws-NORMAL   { background: #14532d; color: var(--green); }
  .ws-LOW      { background: #451a03; color: var(--yellow); }
  .ws-CRITICAL { background: #450a0a; color: var(--red); animation: pulse-red 1s infinite; }
  .ws-EMPTY    { background: #450a0a; color: var(--red); animation: pulse-red 1s infinite; }
  .ws-SENSOR_ERROR { background: #1a1a1a; color: #888; }
  .ws-UNKNOWN  { background: #1a1a1a; color: #888; }
</style>
</head>
<body>

<!-- ============ HEADER ============ -->
<header>
  <div>
    <h1>⚡ HeatPump Controller</h1>
    <div style="font-size:0.78rem;color:var(--text-muted);margin-top:4px;" id="device-alias">Loading...</div>
  </div>
  <div class="meta">
    <span><span class="live-dot"></span>LIVE</span>
    <span id="hdr-ip">IP: —</span>
    <span id="hdr-uptime">Uptime: —</span>
    <span id="hdr-time">—</span>
  </div>
</header>

<div class="grid">

  <!-- ============ DEVICE STATUS ============ -->
  <div class="card">
    <div class="card-title">Device Status</div>
    <div class="status-row">
      <span class="status-label">System State</span>
      <span id="sys-state" class="state-badge state-default">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">WiFi</span>
      <span id="st-wifi"><span class="dot dot-grey"></span>—</span>
    </div>
    <div class="status-row">
      <span class="status-label">PZEM Meter</span>
      <span id="st-pzem"><span class="dot dot-grey"></span>—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Temperature Sensor</span>
      <span id="st-temp"><span class="dot dot-grey"></span>—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Cloud</span>
      <span id="st-cloud"><span class="dot dot-grey"></span>Pending</span>
    </div>
    <div class="status-row">
      <span class="status-label">Relay</span>
      <span id="st-relay"><span class="dot dot-grey"></span>—</span>
    </div>
    <hr/>
    <div class="card-title" style="margin-top:8px;">Active Alarms</div>
    <div id="alarms-container"><p class="no-alarms">✓ No active alarms</p></div>
  </div>

  <!-- ============ ELECTRICAL ============ -->
  <div class="card">
    <div class="card-title">Electrical (3-Phase)</div>
    <div id="pzem-container">
      <p style="color:var(--text-muted);font-size:0.85rem;">Scanning phases...</p>
    </div>
  </div>

  <!-- ============ TEMPERATURE ============ -->
  <div class="card">
    <div class="card-title">Temperature Sensors</div>
    <div id="temp-container">
      <p style="color:var(--text-muted);font-size:0.85rem;">Scanning bus...</p>
    </div>
  </div>

  <!-- ============ CONTROL ============ -->
  <div class="card">
    <div class="card-title">Control</div>

    <div class="status-row">
      <span class="status-label">Setpoint</span>
      <span id="ctrl-setpoint" style="font-weight:600;">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">HP Relay</span>
      <span id="ctrl-relay-hp" style="font-weight:600;">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Heater Relay</span>
      <span id="ctrl-relay-heater" style="font-weight:600;">—</span>
    </div>

    <hr/>
    <div class="card-title" style="margin-top:8px;">Heat Pump</div>
    <div class="btn-row">
      <button class="btn btn-primary"   onclick="sendControl({hp_enabled:true})">HP AUTO</button>
      <button class="btn btn-secondary" onclick="sendControl({hp_enabled:false})">HP OFF</button>
    </div>

    <div class="card-title" style="margin-top:14px;">Heater</div>
    <div class="btn-row">
      <button class="btn btn-warning"   onclick="sendControl({heater_enabled:true})">Heater ON</button>
      <button class="btn btn-secondary" onclick="sendControl({heater_enabled:false})">Heater OFF</button>
    </div>
    <div style="display:grid;grid-template-columns:1fr auto;gap:8px;margin-top:10px;align-items:flex-end;">
      <div>
        <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Heater Stop Temp (°C)</div>
        <input type="number" id="inp-heater-setpoint" min="20" max="90" step="0.5"
          style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
      </div>
      <button class="btn btn-primary" onclick="saveHeaterSetpoint()" style="white-space:nowrap;">Save</button>
    </div>
    <div id="heater-setpoint-msg" style="font-size:0.78rem;margin-top:6px;text-align:center;min-height:16px;color:var(--accent);"></div>

    <hr/>
    <div class="card-title" style="margin-top:8px;">Control Sensor &amp; Setpoints</div>
    <div style="margin-bottom:10px;">
      <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Sensor used for HP &amp; Heater thermostat</div>
      <div style="display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center;">
        <select id="sel-ctrl-sensor"
          style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;">
          <option value="0">Sensor 0 (default)</option>
          <option value="1">Sensor 1</option>
          <option value="2">Sensor 2</option>
        </select>
        <button class="btn btn-primary" onclick="saveCtrlSensor()" style="white-space:nowrap;">Save</button>
      </div>
      <div id="ctrl-sensor-msg" style="font-size:0.78rem;margin-top:6px;text-align:center;min-height:16px;color:var(--accent);"></div>
    </div>
    <div class="card-title" style="margin-top:8px;">Heat Pump Setpoint &amp; Hysteresis</div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px;">
      <div>
        <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Stop Temp (°C)</div>
        <input type="number" id="inp-setpoint" min="20" max="90" step="0.5"
          style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
      </div>
      <div>
        <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Hysteresis (°C)</div>
        <input type="number" id="inp-hysteresis" min="0.5" max="10" step="0.5"
          style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
      </div>
    </div>
    <div style="font-size:0.75rem;color:var(--text-muted);margin-bottom:8px;">
      HP starts at <b id="lbl-start-temp">—</b> °C &nbsp;·&nbsp; HP stops at <b id="lbl-stop-temp">—</b> °C
    </div>
    <button class="btn btn-primary" style="width:100%;" onclick="saveSetpoint()">Save Setpoint</button>
    <div id="setpoint-msg" style="font-size:0.78rem;margin-top:8px;text-align:center;min-height:16px;color:var(--accent);"></div>

    <hr/>
    <div class="btn-row" style="margin-top:4px;">
      <button class="btn btn-danger" onclick="sendControl({mode:'reset'})">Reset Lockout</button>
    </div>
  </div>

  <!-- ============ PRESSURE ============ -->
  <div class="card">
    <div class="card-title">Safety Switches</div>
    <div class="metric-grid">
      <div class="metric">
        <div class="metric-label">High Pressure</div>
        <div class="metric-value" id="hp-status">—</div>
        <div class="metric-unit">Switch</div>
      </div>
      <div class="metric">
        <div class="metric-label">Low Pressure</div>
        <div class="metric-value" id="lp-status">—</div>
        <div class="metric-unit">Switch</div>
      </div>
    </div>
  </div>

  <!-- ============ WATER TANK ============ -->
  <div class="card">
    <div class="card-title" style="display:flex;justify-content:space-between;align-items:center;">
      💧 Water Tank
      <span id="water-online-badge" style="font-size:0.72rem;">—</span>
    </div>

    <!-- Level bar -->
    <div class="water-bar-wrap">
      <div class="water-bar-fill" id="water-bar" style="width:0%"></div>
      <div class="water-bar-label" id="water-bar-label">—</div>
    </div>

    <!-- Metrics row -->
    <div class="metric-grid" style="margin-bottom:12px;">
      <div class="metric">
        <div class="metric-label">Distance</div>
        <div class="metric-value" id="water-dist">—</div>
        <div class="metric-unit">cm (air gap)</div>
      </div>
      <div class="metric">
        <div class="metric-label">Water Level</div>
        <div class="metric-value" id="water-pct">—</div>
        <div class="metric-unit">%</div>
      </div>
      <div class="metric">
        <div class="metric-label">State</div>
        <div class="metric-value" style="font-size:1rem;padding-top:6px;" id="water-state">—</div>
      </div>
    </div>

    <!-- Shutoff warning -->
    <div id="water-shutoff-warn" style="display:none;background:#450a0a;border:1px solid var(--red);color:var(--red);padding:8px 12px;border-radius:6px;font-size:0.8rem;font-weight:600;margin-bottom:12px;">
      🚨 WATER LOW — Heat Pump Shutoff Active
    </div>

    <hr/>

    <!-- Tank configuration -->
    <div class="card-title" style="margin-top:8px;">Tank Calibration</div>
    <div style="background:var(--surface2);border:1px solid var(--border);border-radius:8px;padding:12px;margin-bottom:12px;font-size:0.8rem;color:var(--text-muted);line-height:1.8;">
      <b style="color:var(--text);">How to calibrate:</b><br/>
      1️⃣ Empty your tank completely → click <b style="color:var(--green);">Set as EMPTY</b><br/>
      2️⃣ Fill your tank completely → click <b style="color:var(--accent);">Set as FULL</b>
    </div>

    <!-- Big live distance for calibration reference -->
    <div style="text-align:center;background:var(--surface2);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:12px;">
      <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">LIVE DISTANCE RIGHT NOW</div>
      <div style="font-size:2.8rem;font-weight:700;color:var(--accent);" id="calib-live-dist">—</div>
      <div style="font-size:0.75rem;color:var(--text-muted);">cm</div>
    </div>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px;">
      <div>
        <button class="btn btn-secondary" style="width:100%;padding:14px;font-size:0.9rem;" onclick="calibratePoint('empty')">
          💭 Set as EMPTY
        </button>
        <div id="calib-empty-label" style="font-size:0.72rem;color:var(--text-muted);text-align:center;margin-top:5px;">Empty: —</div>
      </div>
      <div>
        <button class="btn btn-primary" style="width:100%;padding:14px;font-size:0.9rem;" onclick="calibratePoint('full')">
          💧 Set as FULL
        </button>
        <div id="calib-full-label" style="font-size:0.72rem;color:var(--text-muted);text-align:center;margin-top:5px;">Full: —</div>
      </div>
    </div>
    <div id="tank-cfg-msg" style="font-size:0.78rem;margin-bottom:8px;text-align:center;min-height:16px;color:var(--accent);"></div>

    <hr/>
    <!-- Advanced manual override -->
    <details style="margin-top:8px;">
      <summary style="font-size:0.75rem;color:var(--text-muted);cursor:pointer;user-select:none;">Advanced: type values manually</summary>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px;margin-bottom:10px;">
        <div>
          <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Empty Distance (cm)</div>
          <input type="number" id="cfg-tank-height" min="1" max="600" step="0.5"
            style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
        </div>
        <div>
          <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Full Distance (cm)</div>
          <input type="number" id="cfg-sensor-offset" min="0" max="100" step="0.5"
            style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
        </div>
        <div>
          <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Shutoff Level (%)</div>
          <input type="number" id="cfg-water-shutoff" min="5" max="40" step="1"
            style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
        </div>
        <div>
          <div style="font-size:0.72rem;color:var(--text-muted);margin-bottom:4px;">Low Alarm Level (%)</div>
          <input type="number" id="cfg-water-low" min="10" max="60" step="1"
            style="width:100%;padding:8px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;font-size:0.9rem;"/>
        </div>
      </div>
      <button class="btn btn-secondary" style="width:100%;" onclick="saveTankConfig()">Save Manual Values</button>
    </details>
  </div>

  <!-- ============ HEALTH ============ -->
  <div class="card">
    <div class="card-title">System Health</div>
    <div class="status-row">
      <span class="status-label">Free Heap</span>
      <span id="h-heap">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Min Free Heap</span>
      <span id="h-frag">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">WiFi RSSI</span>
      <span id="h-rssi">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Firmware</span>
      <span id="h-fw">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Device ID</span>
      <span id="h-devid" style="font-family:monospace;font-size:0.8rem;">—</span>
    </div>
  </div>

  <!-- ============ NETWORK SETUP ============ -->
  <div class="card">
    <div class="card-title">WiFi Setup</div>
    <div style="margin-bottom:12px;">
      <p style="font-size:0.8rem;color:var(--text-muted);margin-bottom:12px;">Configure WiFi to upload telemetry. If unavailable, system automatically falls back to Cellular GPRS.</p>
      <input type="text" id="wifi-ssid" autocomplete="off" placeholder="WiFi Network Name (SSID)" style="width:100%;padding:10px;margin-bottom:10px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;"/>
      <input type="password" id="wifi-pass" autocomplete="new-password" placeholder="WiFi Password" style="width:100%;padding:10px;margin-bottom:12px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;font-family:inherit;"/>
      <button class="btn btn-primary" style="width:100%;padding:10px;" onclick="saveWifi()">Save & Connect</button>
      <div id="wifi-status-msg" style="font-size:0.8rem;margin-top:10px;color:var(--accent);text-align:center;min-height:16px;"></div>
    </div>
  </div>

  <!-- ============ LOGS ============ -->
  <div class="card grid-wide">
    <div class="card-title" style="display:flex;justify-content:space-between;align-items:center;">
      System Log
      <button class="btn btn-secondary" onclick="fetchLogs()" style="padding:4px 12px;font-size:0.72rem;">Refresh</button>
    </div>
    <div class="log-list" id="log-list">
      <p style="color:var(--text-muted);">Loading logs...</p>
    </div>
  </div>

</div><!-- /grid -->

<script>
  // ─────────────────────────────────────────────
  //  BOOT: load static info once
  // ─────────────────────────────────────────────
  window.addEventListener('DOMContentLoaded', () => {
    fetchInfo();
    fetchHealth();
    fetchLogs();
    startPolling();
  });

  // ─────────────────────────────────────────────
  //  ONE-TIME FETCHES
  // ─────────────────────────────────────────────
  function fetchInfo() {
    fetch('/api/info').then(r => r.json()).then(d => {
      document.getElementById('device-alias').textContent =
        `${d.alias}  ·  S/N: ${d.serial_number}  ·  ${d.customer}`;
      document.getElementById('h-devid').textContent = d.device_id;
      document.getElementById('h-fw').textContent =
        `FW ${d.firmware}  HW ${d.hardware}`;
    }).catch(() => {});
  }

  // ─────────────────────────────────────────────
  //  POLLING  (/api/live every 1s)
  // ─────────────────────────────────────────────
  function startPolling() {
    fetchLive();
    setInterval(fetchLive, 1000);
    setInterval(fetchHealth, 5000);
  }

  function fetchLive() {
    fetch('/api/live').then(r => r.json()).then(renderLive).catch(() => {
      setOffline();
    });
  }

  function renderLive(d) {
    // Header
    document.getElementById('hdr-ip').textContent = `IP: ${d.wifi?.ip ?? '—'}`;
    document.getElementById('hdr-uptime').textContent =
      `Uptime: ${formatUptime(d.uptime)}`;

    // State
    const stateEl = document.getElementById('sys-state');
    stateEl.textContent = d.state ?? '—';
    stateEl.className = 'state-badge state-' + (d.state ?? 'default');

    // WiFi
    const wifiOk = d.wifi?.connected;
    document.getElementById('st-wifi').innerHTML =
      dotHtml(wifiOk) + (wifiOk ? `Online (${d.wifi.rssi} dBm)` : 'Offline');

    // PZEM
    const pzemArr = d.pzem || [];
    let pzemHtml = '';
    let allPzemOk = false;

    if (pzemArr.length > 0) {
      allPzemOk = true;
      pzemArr.forEach((p, idx) => {
        if (!p.online) allPzemOk = false;
        
        let phaseName = idx === 0 ? "R Phase" : (idx === 1 ? "Y Phase" : (idx === 2 ? "B Phase" : `Phase ${idx+1}`));
        let statusClass = p.online ? "online" : "offline";
        let statusText = p.online ? "🟢" : "🔴";
        
        pzemHtml += `
          <div style="margin-bottom: 20px; border-bottom: 1px solid var(--border); padding-bottom: 10px;">
            <div style="font-weight:600; margin-bottom:8px; color:var(--text);">
              ${statusText} ${phaseName} <span style="font-size:0.8rem; font-weight:normal;" class="${statusClass}">${p.online ? 'ONLINE' : 'OFFLINE'}</span>
            </div>
            <div class="metric-grid">
              <div class="metric"><div class="metric-label">Voltage</div><div class="metric-value">${p.online ? p.voltage : '—'}</div><div class="metric-unit">V</div></div>
              <div class="metric"><div class="metric-label">Current</div><div class="metric-value">${p.online ? p.current : '—'}</div><div class="metric-unit">A</div></div>
              <div class="metric"><div class="metric-label">Power</div><div class="metric-value">${p.online ? p.power : '—'}</div><div class="metric-unit">W</div></div>
              <div class="metric"><div class="metric-label">Energy</div><div class="metric-value">${p.online ? p.energy : '—'}</div><div class="metric-unit">kWh</div></div>
              <div class="metric"><div class="metric-label">Frequency</div><div class="metric-value">${p.online ? p.frequency : '—'}</div><div class="metric-unit">Hz</div></div>
              <div class="metric"><div class="metric-label">Power Factor</div><div class="metric-value">${p.online ? p.power_factor : '—'}</div><div class="metric-unit">PF</div></div>
            </div>
          </div>
        `;
      });
    } else {
      pzemHtml = '<p style="color:var(--text-muted);font-size:0.85rem;">No phases connected.</p>';
    }
    
    document.getElementById('pzem-container').innerHTML = pzemHtml;
    document.getElementById('st-pzem').innerHTML = dotHtml(allPzemOk) + (allPzemOk ? 'Online' : 'Offline/Partial');

    // Temperature
    const temps = d.temps ?? [];
    const tempOk = temps.some(t => t.online);
    document.getElementById('st-temp').innerHTML =
      dotHtml(tempOk) + (tempOk ? `${temps.filter(t=>t.online).length} sensor(s)` : 'Offline');
    renderTempCards(temps, d.control_sensor_idx ?? 0);

    // Relay HP
    const hpOn = d.relay_hp === 'ON';
    document.getElementById('st-relay').innerHTML =
      `<span class="dot ${hpOn ? 'dot-green' : 'dot-grey'}"></span>HP: ${d.relay_hp ?? '—'}`;
    document.getElementById('ctrl-relay-hp').innerHTML =
      `<span style="color:${hpOn ? 'var(--green)' : 'var(--text-muted)'}">${d.relay_hp ?? '—'}</span>`;

    // Relay Heater
    const htOn = d.relay_heater === 'ON';
    document.getElementById('ctrl-relay-heater').innerHTML =
      `<span style="color:${htOn ? 'var(--yellow)' : 'var(--text-muted)'}">${d.relay_heater ?? '—'}</span>`;
    
    // Setpoint display — populate input boxes once
    if (d.setpoint !== undefined) {
      document.getElementById('ctrl-setpoint').textContent = d.setpoint + ' °C';
      const hyst = d.hysteresis ?? 2.0;
      document.getElementById('lbl-start-temp').textContent = (d.setpoint - hyst).toFixed(1);
      document.getElementById('lbl-stop-temp').textContent  = d.setpoint.toFixed(1);
      if (!document.getElementById('inp-setpoint').dataset.populated) {
        document.getElementById('inp-setpoint').value    = d.setpoint;
        document.getElementById('inp-hysteresis').value  = hyst;
        document.getElementById('inp-setpoint').dataset.populated = '1';
      }
    }
    if (d.heater_setpoint !== undefined) {
      if (!document.getElementById('inp-heater-setpoint').dataset.populated) {
        document.getElementById('inp-heater-setpoint').value = d.heater_setpoint;
        document.getElementById('inp-heater-setpoint').dataset.populated = '1';
      }
    }
    if (d.control_sensor_idx !== undefined) {
      const sel = document.getElementById('sel-ctrl-sensor');
      if (!sel.dataset.populated) {
        sel.value = d.control_sensor_idx;
        sel.dataset.populated = '1';
      }
    }

    // Safety Switches
    if (d.safety) {
      const hpEl = document.getElementById('hp-status');
      const lpEl = document.getElementById('lp-status');
      hpEl.textContent = d.safety.hp_fault ? 'FAULT' : 'SAFE';
      hpEl.style.color = d.safety.hp_fault ? 'var(--red)' : 'var(--green)';
      lpEl.textContent = d.safety.lp_fault ? 'FAULT' : 'SAFE';
      lpEl.style.color = d.safety.lp_fault ? 'var(--red)' : 'var(--green)';
    }

    // Water tank
    if (d.water) renderWater(d.water);

    // Alarms
    renderAlarms(d.alarms ?? []);
  }

  function renderTempCards(temps, ctrlIdx) {
    const container = document.getElementById('temp-container');
    if (!temps.length) {
      container.innerHTML = '<p style="color:var(--text-muted);font-size:0.85rem;">No sensors detected</p>';
      return;
    }
    container.innerHTML = temps.map((s, i) => `
      <div class="metric-grid" style="margin-bottom:10px;">
        <div class="metric">
          <div class="metric-label">${s.name}${i === ctrlIdx ? ' <span style="color:var(--yellow);font-size:0.65rem;">★ CONTROL</span>' : ''}</div>
          <div class="metric-value" style="font-size:2rem;">${s.online ? s.value : '—'}</div>
          <div class="metric-unit">°C</div>
          <div class="metric-status ${s.online ? 'online' : 'offline'}">${s.online ? 'ONLINE' : 'OFFLINE'}</div>
        </div>
      </div>`).join('');
  }

  function renderAlarms(alarms) {
    const el = document.getElementById('alarms-container');
    if (!alarms.length) {
      el.innerHTML = '<p class="no-alarms">✓ No active alarms</p>';
    } else {
      el.innerHTML = '<div class="alarm-list">' +
        alarms.map(a => `<span class="alarm-pill">${a}</span>`).join('') +
        '</div>';
    }
  }

  // ─────────────────────────────────────────────
  //  WATER TANK
  // ─────────────────────────────────────────────
  let _tankCfgPopulated = false;

  function renderWater(w) {
    const online  = w.sensor_online;
    const pct     = w.level_pct  ?? 0;
    const dist    = w.distance_cm ?? 0;
    const state   = w.level_state ?? 'UNKNOWN';
    const shutoff = w.shutoff_active;

    // Online badge
    document.getElementById('water-online-badge').innerHTML =
      online ? '<span class="dot dot-green"></span><span style="color:var(--green);font-size:0.72rem;">ONLINE</span>'
             : '<span class="dot dot-red"></span><span style="color:var(--red);font-size:0.72rem;">OFFLINE</span>';

    // Metrics
    document.getElementById('water-dist').textContent = online ? dist.toFixed(1) : '—';
    document.getElementById('water-pct').textContent  = online ? pct.toFixed(1)  : '—';

    // State badge
    const stateEl = document.getElementById('water-state');
    stateEl.innerHTML = `<span class="water-state-badge ws-${state}">${state}</span>`;

    // Level bar
    const bar = document.getElementById('water-bar');
    const barLabel = document.getElementById('water-bar-label');
    bar.style.width = online ? pct + '%' : '0%';
    barLabel.textContent = online ? pct.toFixed(1) + '%' : 'Sensor Offline';
    bar.className = 'water-bar-fill';
    if      (state === 'CRITICAL' || state === 'EMPTY') bar.classList.add('critical');
    else if (state === 'LOW')                           bar.classList.add('low');

    // Shutoff warning banner
    document.getElementById('water-shutoff-warn').style.display = shutoff ? 'block' : 'none';

    // Update calibration live display
    const calibEl = document.getElementById('calib-live-dist');
    if (calibEl) calibEl.textContent = online ? dist.toFixed(1) : '—';

    // Populate config inputs ONCE with current values from ESP32
    if (!_tankCfgPopulated && w.empty_dist_cm !== undefined) {
      document.getElementById('cfg-tank-height').value    = w.empty_dist_cm;
      document.getElementById('cfg-sensor-offset').value  = w.full_dist_cm;
      document.getElementById('cfg-water-shutoff').value  = w.water_shutoff_pct;
      document.getElementById('cfg-water-low').value      = w.water_low_alarm_pct;
      _tankCfgPopulated = true;
    }
  }

  // ── One-click calibration ────────────────────────────────────────────
  function calibratePoint(point) {
    const msg = document.getElementById('tank-cfg-msg');
    msg.style.color = 'var(--accent)';
    msg.textContent = 'Saving calibration point...';
    fetch('/api/calibrate/' + point, { method: 'POST' })
      .then(r => r.json())
      .then(d => {
        if (d.error) {
          msg.style.color = 'var(--red)';
          msg.textContent = '⚠️ ' + d.error;
          return;
        }
        msg.style.color = 'var(--green)';
        if (point === 'empty') {
          msg.textContent = '✓ Empty point saved at ' + d.empty_dist_cm + ' cm';
          document.getElementById('calib-empty-label').textContent = 'Empty: ' + d.empty_dist_cm + ' cm';
        } else {
          msg.textContent = '✓ Full point saved at ' + d.full_dist_cm + ' cm';
          document.getElementById('calib-full-label').textContent = 'Full: ' + d.full_dist_cm + ' cm';
        }
        _tankCfgPopulated = false;   // refresh inputs on next poll
        setTimeout(() => msg.textContent = '', 5000);
      })
      .catch(() => {
        msg.style.color = 'var(--red)';
        msg.textContent = 'Error connecting to sensor.';
      });
  }

  function saveTankConfig() {
    const h  = parseFloat(document.getElementById('cfg-tank-height').value);
    const o  = parseFloat(document.getElementById('cfg-sensor-offset').value);
    const s  = parseFloat(document.getElementById('cfg-water-shutoff').value);
    const l  = parseFloat(document.getElementById('cfg-water-low').value);
    const msg = document.getElementById('tank-cfg-msg');

    if (isNaN(h) || h <= 0)   { msg.style.color='var(--red)'; msg.textContent='Invalid tank height'; return; }
    if (isNaN(o) || o < 0)    { msg.style.color='var(--red)'; msg.textContent='Invalid offset'; return; }
    if (isNaN(s) || s < 5)    { msg.style.color='var(--red)'; msg.textContent='Shutoff must be ≥ 5%'; return; }
    if (isNaN(l) || l <= s)   { msg.style.color='var(--red)'; msg.textContent='Low alarm must be > shutoff %'; return; }

    msg.style.color = 'var(--accent)';
    msg.textContent = 'Saving...';

    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        empty_dist_cm:       h,
        full_dist_cm:        o,
        water_shutoff_pct:   s,
        water_low_alarm_pct: l
      })
    }).then(r => r.json()).then(d => {
      msg.style.color = 'var(--green)';
      msg.textContent = '✓ Tank settings saved!';
      _tankCfgPopulated = false;  // repopulate inputs from next live response
      setTimeout(() => msg.textContent = '', 4000);
    }).catch(() => {
      msg.style.color = 'var(--red)';
      msg.textContent = 'Error saving. Try again.';
    });
  }

  // ─────────────────────────────────────────────
  //  HEALTH  (every 5s)
  // ─────────────────────────────────────────────
  function fetchHealth() {
    fetch('/api/health').then(r => r.json()).then(d => {
      document.getElementById('h-heap').textContent =
        `${(d.free_heap / 1024).toFixed(1)} KB`;
      document.getElementById('h-frag').textContent =
        `${(d.min_free_heap / 1024).toFixed(1)} KB`;
      document.getElementById('h-rssi').textContent =
        `${d.wifi_rssi} dBm`;
    }).catch(() => {});
  }

  // ─────────────────────────────────────────────
  //  LOGS
  // ─────────────────────────────────────────────
  function fetchLogs() {
    fetch('/api/logs').then(r => r.json()).then(d => {
      const list = document.getElementById('log-list');
      const entries = d.logs ?? [];
      if (!entries.length) {
        list.innerHTML = '<p style="color:var(--text-muted);">No log entries.</p>';
        return;
      }
      list.innerHTML = entries.slice(-50).reverse().map(e => `
        <div class="log-entry">
          <span class="log-ts">[${formatMs(e.ts)}]</span>
          <span class="log-${e.level}">[${e.level}]</span>
          <span class="log-msg">${e.msg}</span>
        </div>`).join('');
    }).catch(() => {});
  }

  // ─────────────────────────────────────────────
  //  CONTROL
  // ─────────────────────────────────────────────
  function sendControl(cmd) {
    fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cmd)
    }).then(r => r.json()).then(d => {
      console.log('Control:', d);
    }).catch(e => console.error(e));
  }

  function saveSetpoint() {
    const sp   = parseFloat(document.getElementById('inp-setpoint').value);
    const hyst = parseFloat(document.getElementById('inp-hysteresis').value);
    const msg  = document.getElementById('setpoint-msg');
    if (isNaN(sp) || sp < 20 || sp > 90)   { msg.style.color='var(--red)'; msg.textContent='Setpoint must be 20–90°C'; return; }
    if (isNaN(hyst) || hyst < 0.5 || hyst > 10) { msg.style.color='var(--red)'; msg.textContent='Hysteresis must be 0.5–10°C'; return; }
    msg.style.color = 'var(--accent)';
    msg.textContent = 'Saving...';
    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ temp_setpoint: sp, temp_hysteresis: hyst })
    }).then(r => r.json()).then(() => {
      msg.style.color = 'var(--green)';
      msg.textContent = `✓ Saved! HP starts at ${(sp - hyst).toFixed(1)}°C, stops at ${sp.toFixed(1)}°C`;
      document.getElementById('inp-setpoint').dataset.populated = '';  // refresh labels
      setTimeout(() => msg.textContent = '', 5000);
    }).catch(() => {
      msg.style.color = 'var(--red)';
      msg.textContent = 'Error saving. Try again.';
    });
  }

  function saveHeaterSetpoint() {
    const sp  = parseFloat(document.getElementById('inp-heater-setpoint').value);
    const msg = document.getElementById('heater-setpoint-msg');
    if (isNaN(sp) || sp < 20 || sp > 90) { msg.style.color='var(--red)'; msg.textContent='Heater setpoint must be 20–90°C'; return; }
    msg.style.color = 'var(--accent)';
    msg.textContent = 'Saving...';
    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ heater_setpoint: sp })
    }).then(r => r.json()).then(() => {
      msg.style.color = 'var(--green)';
      msg.textContent = `✓ Heater will stop at ${sp.toFixed(1)}°C`;
      document.getElementById('inp-heater-setpoint').dataset.populated = '';  // refresh
      setTimeout(() => msg.textContent = '', 5000);
    }).catch(() => {
      msg.style.color = 'var(--red)';
      msg.textContent = 'Error saving. Try again.';
    });
  }

  function saveCtrlSensor() {
    const idx = parseInt(document.getElementById('sel-ctrl-sensor').value);
    const msg = document.getElementById('ctrl-sensor-msg');
    msg.style.color = 'var(--accent)';
    msg.textContent = 'Saving...';
    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ control_sensor_idx: idx })
    }).then(r => r.json()).then(() => {
      msg.style.color = 'var(--green)';
      msg.textContent = `✓ Sensor ${idx} is now the control sensor.`;
      document.getElementById('sel-ctrl-sensor').dataset.populated = ''; // refresh
      setTimeout(() => msg.textContent = '', 5000);
    }).catch(() => {
      msg.style.color = 'var(--red)';
      msg.textContent = 'Error saving. Try again.';
    });
  }

  // ─────────────────────────────────────────────
  //  NETWORK SETUP
  // ─────────────────────────────────────────────
  function saveWifi() {
    const ssid = document.getElementById('wifi-ssid').value;
    const pass = document.getElementById('wifi-pass').value;
    if (!ssid) return;
    
    document.getElementById('wifi-status-msg').textContent = 'Saving...';
    fetch('/api/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: ssid, pass: pass })
    }).then(r => r.json()).then(d => {
      document.getElementById('wifi-status-msg').textContent = 'Saved! ESP32 is connecting...';
      document.getElementById('wifi-ssid').value = '';
      document.getElementById('wifi-pass').value = '';
      setTimeout(() => { document.getElementById('wifi-status-msg').textContent = ''; }, 5000);
    }).catch(e => {
      document.getElementById('wifi-status-msg').textContent = 'Error saving credentials.';
    });
  }

  // ─────────────────────────────────────────────
  //  HELPERS
  // ─────────────────────────────────────────────
  function dotHtml(ok) {
    return `<span class="dot ${ok ? 'dot-green' : 'dot-red'}"></span>`;
  }

  function setText(id, val) {
    const el = document.getElementById(id);
    if (el) el.textContent = val ?? '—';
  }

  function formatUptime(sec) {
    if (!sec) return '—';
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    return `${h}h ${m}m ${s}s`;
  }

  function formatMs(ms) {
    const s = Math.floor(ms / 1000);
    return formatUptime(s);
  }

  function setOffline() {
    document.getElementById('sys-state').textContent = 'DISCONNECTED';
    document.getElementById('sys-state').className = 'state-badge state-FAULT';
  }
</script>
</body>
</html>
)=====";
