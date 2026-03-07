#ifndef INDEX_HTML_H
#define INDEX_HTML_H

const char *htmlPage = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>Pedal Config Pro</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: 'Segoe UI', Roboto, sans-serif; text-align: center; background-color: #121212; color: #e0e0e0; padding: 10px; margin: 0; }
    h2 { color: #fff; margin: 5px 0; font-size: 22px; letter-spacing: 1px; }
    .card { background-color: #1e1e1e; padding: 10px; border-radius: 8px; max-width: 550px; margin: 10px auto; box-shadow: 0 4px 6px rgba(0,0,0,0.4); }
    .btn-card { background-color: #252525; border: 1px solid #333; border-radius: 8px; margin-bottom: 15px; padding: 10px; text-align: left;}
    .btn-header { font-size: 16px; font-weight: bold; color: #2196F3; border-bottom: 1px solid #444; padding-bottom: 5px; margin-bottom: 10px; }
    input, select { background-color: #333; color: #fff; border: 1px solid #444; padding: 8px 4px; font-size: 14px; border-radius: 4px; text-align: center; font-weight: bold; }
    input { width: 45px; } input.sm { width: 35px; } select { width: 75px; }
    .settings-row { display: flex; justify-content: space-between; flex-wrap: wrap; margin-bottom: 8px; }
    .input-group { display: flex; flex-direction: column; align-items: center; margin-right: 5px; }
    .input-group label { font-size: 10px; color: #888; margin-bottom: 2px; text-transform: uppercase; }
    .conn-row { background: #1a1a1a; padding: 6px; border-radius: 4px; font-size: 13px; color: #aaa; margin-bottom: 8px;}
    .opt-header { font-size: 13px; color: #ccc; margin-top: 5px; border-top: 1px solid #444; padding-top: 5px; cursor: pointer;}
    .opt-settings { background: #2f271d; margin-top: 5px; padding: 8px; border-radius: 4px; border: 1px solid #555; }
    .btn-pair, .btn-reboot, .btn-load, .btn-save { padding: 12px; color: white; border: none; border-radius: 6px; cursor: pointer; font-weight: bold; margin: 5px; text-decoration: none; display:inline-block; width: 100px;}
    .btn-pair { background: #2196F3; width: auto; } .btn-reboot { background: #d9534f; width: 100%;} .btn-load { background: #4CAF50; } .btn-save { background: #FF9800; }
  </style>
  <script>
    var presets = %PRESET_JSON%; 
    var liveSettings = {}; 
    function toggleBox(id, checked) {
       document.getElementById(id).style.display = checked ? 'block' : 'none';
    }
    function sendData(element) {
      var key = element.name; var val = element.value;
      sendDataRaw(key, val, element);
    }
    function sendDataRaw(key, val, element) {
      if(element) element.style.borderColor = "#FFFF00"; 
      var xhr = new XMLHttpRequest(); xhr.open("GET", "/save?" + key + "=" + val, true);
      xhr.onreadystatechange = function() {
        if (xhr.readyState == 4 && xhr.status == 200) {
          if(element) { element.style.borderColor = "#00FF00"; setTimeout(function(){ element.style.borderColor = "#444"; }, 600); }
        }
      };
      xhr.send();
    }
    function loadPreset() {
      var slot = document.getElementById("p_slot").value;
      if (slot == "0") return;
      if(confirm("Load Preset " + slot + "?")) { window.location.href = "/preset?act=load&slot=" + slot; }
    }
    function savePreset() {
      var slot = document.getElementById("p_slot").value;
      if (slot == "0") { alert("Select a Preset 1-3."); return; }
      var name = prompt("Name for Preset " + slot + ":", presets[slot] ? presets[slot].name : "Preset " + slot);
      if (name) { window.location.href = "/preset?act=save&slot=" + slot + "&name=" + encodeURIComponent(name); }
    }
    function saveChannel(ch) {
      var sel = document.getElementById("ch_sel");
      sel.style.borderColor = "#FFFF00";
      var xhr = new XMLHttpRequest();
      xhr.open("GET", "/savech?ch=" + ch, true);
      xhr.onreadystatechange = function() {
        if (xhr.readyState == 4 && xhr.status == 200) {
          sel.style.borderColor = "#00FF00";
          setTimeout(function(){ sel.style.borderColor = "#444"; }, 600);
          if(ch == "0") {
            document.getElementById("ch_status").innerText = "Mode: Auto-Scan (reboot to apply)";
          } else {
            document.getElementById("ch_status").innerText = "Channel " + ch + " set (reboot to apply)";
          }
        }
      };
      xhr.send();
    }
    function reboot() { if(confirm("Restart pedal?")) { var xhr = new XMLHttpRequest(); xhr.open("GET", "/reboot", true); xhr.send(); alert("Rebooting..."); } }
    function initChannel() {
      var auto = %AUTO_CH%;
      var ch = %WIFI_CH%;
      var sel = document.getElementById("ch_sel");
      if(auto == 1) { sel.value = "0"; } else { sel.value = ch; }
    }
    window.onload = initChannel;
  </script>
</head>
<body>
  <div class="card" style="background:transparent;box-shadow:none;padding:5px"><h2>MIDI CONFIG PRO</h2></div>
  <div class="card">
    <select id="p_slot" style="width:100%; margin-bottom:10px; text-align:left"><option value="0">-- Active Settings --</option><option value="1">1: %P1_NAME%</option><option value="2">2: %P2_NAME%</option><option value="3">3: %P3_NAME%</option><option value="4">4: %P4_NAME%</option></select>
    <div style="display:flex; justify-content:center"><button class="btn-load" onclick="loadPreset()">LOAD</button><button class="btn-save" onclick="savePreset()">SAVE</button></div>
    
    <div style="border-top:1px solid #333; margin-top:10px; padding-top:10px; text-align:center">
      <span style="font-size:12px; color:#888; text-transform:uppercase; display:block; margin-bottom:5px">Global Timing Settings</span>
      <span style="font-size:12px; color:#2196F3">Double Press (ms):</span> <input name="dp_time" value="%DP_TIME%" onchange="sendData(this)" style="width:50px">
      <span style="font-size:12px; color:#FF9800; margin-left:10px">Long Press (ms):</span> <input name="lp_time" value="%LP_TIME%" onchange="sendData(this)" style="width:50px">
    </div>
  </div>
  <div class="card" style="background:transparent; padding:0; box-shadow:none">%BUTTON_CARDS%</div>
  <div class="card">
    <p>Target: <b>%CURr_MAC%</b></p> %PAIR_STATUS%
    <div style="border-top:1px solid #333; margin-top:10px; padding-top:10px;">
      <span style="font-size:12px; color:#888; text-transform:uppercase; display:block; margin-bottom:5px">WiFi Channel (ESP-NOW)</span>
      <select id="ch_sel" onchange="saveChannel(this.value)" style="width:100%">
        <option value="0">Auto (Scan 1-11)</option>
        <option value="1">Channel 1</option>
        <option value="2">Channel 2</option>
        <option value="3">Channel 3</option>
        <option value="4">Channel 4</option>
        <option value="5">Channel 5</option>
        <option value="6">Channel 6</option>
        <option value="7">Channel 7</option>
        <option value="8">Channel 8</option>
        <option value="9">Channel 9</option>
        <option value="10">Channel 10</option>
        <option value="11">Channel 11</option>
      </select>
      <div id="ch_status" style="font-size:11px; color:#666; margin-top:5px">Current: Ch %WIFI_CH% (%AUTO_CH% == 1 ? "Auto" : "Manual")</div>
    </div>
    <br><button class="btn-reboot" onclick="reboot()">REBOOT PEDAL</button>
  </div>
</body></html>
)rawliteral";

#endif
