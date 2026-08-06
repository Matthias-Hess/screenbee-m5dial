#pragma once

// Lean setup HTML - WiFi + MQTT only, no project upload tab (that's
// checkpoint 4's job, tied to a ProjectInstaller that doesn't exist yet
// for this device). Styling/JS structure mirrors MqttEPaperDisplay2's
// UnifiedConfiguratorHTML.h closely (same look across ScreenBee devices),
// just with the "Project Upload" tab and its handlers removed.

static const char WIFI_SETUP_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ScreenBee M5 Dial Setup</title><style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px}
.container{max-width:600px;margin:0 auto;background:white;border-radius:16px;box-shadow:0 20px 60px rgba(0,0,0,0.3);overflow:hidden}
.header{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;padding:30px;text-align:center}
.header h1{font-size:28px;margin-bottom:5px}
.header p{opacity:0.9;font-size:14px}
.tabs{display:flex;border-bottom:2px solid #f0f0f0}
.tab{flex:1;padding:20px;text-align:center;cursor:pointer;background:white;border:none;font-size:16px;font-weight:600;color:#666;transition:all 0.3s}
.tab:hover{background:#f8f9ff}
.tab.active{color:#667eea;border-bottom:3px solid #667eea}
.tab-content{display:none;padding:30px}
.tab-content.active{display:block}
.form-group{margin-bottom:20px}
label{display:block;margin-bottom:8px;font-weight:600;color:#333}
input,select{width:100%;padding:12px;border:2px solid #e0e0e0;border-radius:8px;font-size:15px;transition:border-color 0.3s}
input:focus,select:focus{outline:none;border-color:#667eea}
.btn{width:100%;padding:15px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:transform 0.2s}
.btn:hover{transform:translateY(-2px);box-shadow:0 10px 25px rgba(102,126,234,0.4)}
.btn:active{transform:translateY(0)}
.btn:disabled{opacity:0.5;cursor:not-allowed}
.message{padding:15px;border-radius:8px;margin-bottom:20px;display:none}
.message.show{display:block}
.message.success{background:#e8f5e9;color:#2e7d32;border-left:4px solid #4caf50}
.message.error{background:#ffebee;color:#c62828;border-left:4px solid #f44336}
.network-list{max-height:300px;overflow-y:auto;border:2px solid #e0e0e0;border-radius:8px;margin-bottom:20px}
.network-item{padding:15px;border-bottom:1px solid #f0f0f0;cursor:pointer;transition:background 0.2s}
.network-item:hover{background:#f8f9ff}
.network-item:last-child{border-bottom:none}
.network-name{font-weight:600;color:#333}
.network-signal{font-size:13px;color:#666;margin-top:3px}
.loading{text-align:center;padding:20px;color:#667eea}
</style></head><body>
<div class="container">
<div class="header"><h1>ScreenBee Setup</h1><p>M5 Dial - Configure WiFi and MQTT</p></div>
<div class="tabs">
<button class="tab active" onclick="switchTab('wifi')">WiFi Setup</button>
<button class="tab" onclick="switchTab('mqtt')">MQTT Connection</button>
</div>
<div id="wifi-tab" class="tab-content active">
<div id="wifi-message" class="message"></div>
<div class="form-group"><label>Available Networks</label>
<div id="network-list" class="network-list loading">Scanning networks...</div></div>
<div class="form-group"><label for="ssid">Network Name (SSID)</label>
<input type="text" id="ssid" placeholder="Enter WiFi network name" required></div>
<div class="form-group"><label for="password">Password</label>
<input type="password" id="password" placeholder="Enter WiFi password" required></div>
<button class="btn" onclick="saveWiFi()">Save WiFi Settings</button>
</div>
<div id="mqtt-tab" class="tab-content">
<div id="mqtt-message" class="message"></div>
<div class="form-group">
<div style="display:flex;gap:10px;align-items:end">
<div style="width:20%"><label for="mqttProtocol">Protocol</label>
<select id="mqttProtocol" style="width:100%">
<option value="mqtt://">mqtt://</option>
<option value="mqtts://">mqtts://</option>
</select></div>
<div style="width:60%"><label for="mqttHost">Host</label>
<input type="text" id="mqttHost" placeholder="192.168.1.107" required style="width:100%"></div>
<div style="width:20%"><label for="mqttPort">Port</label>
<input type="number" id="mqttPort" placeholder="1883" min="1" max="65535" required style="width:100%"></div>
</div></div>
<div class="form-group"><label for="mqttUsername">Username</label>
<input type="text" id="mqttUsername" placeholder="MQTT username (optional)"></div>
<div class="form-group"><label for="mqttPassword">Password</label>
<input type="password" id="mqttPassword" placeholder="MQTT password (optional)"></div>
<div style="display:flex;gap:10px">
<button class="btn" onclick="saveMqtt()" style="flex:1">Save MQTT Settings</button>
<button class="btn" onclick="testMqtt()" style="flex:1;background:linear-gradient(135deg,#4caf50 0%,#45a049 100%)">Test MQTT Connection</button>
</div>
</div>
</div>
<script>
function switchTab(tab){
document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active')});
document.querySelectorAll('.tab-content').forEach(function(c){c.classList.remove('active')});
if(tab==='wifi'){
document.querySelectorAll('.tab')[0].classList.add('active');
document.getElementById('wifi-tab').classList.add('active');
}else{
document.querySelectorAll('.tab')[1].classList.add('active');
document.getElementById('mqtt-tab').classList.add('active');
}}
window.addEventListener('load',scanNetworks);
function scanNetworks(){
fetch('/api/scan').then(function(r){return r.json()}).then(function(data){
var list=document.getElementById('network-list');
list.classList.remove('loading');
list.innerHTML='';
var networks=JSON.parse(data.message).networks;
if(networks.length===0){
list.innerHTML='<div style="padding:20px;text-align:center;color:#666">No networks found</div>';
return;}
networks.forEach(function(network){
var item=document.createElement('div');
item.className='network-item';
item.onclick=function(){document.getElementById('ssid').value=network.ssid};
var signal=network.rssi;
var signalStr=signal>-50?'Excellent':signal>-60?'Good':signal>-70?'Fair':'Weak';
item.innerHTML='<div class="network-name">'+network.ssid+'</div><div class="network-signal">Signal: '+signalStr+' ('+signal+' dBm) - '+network.encryption+'</div>';
list.appendChild(item);
});}).catch(function(err){
document.getElementById('network-list').innerHTML='<div style="padding:20px;text-align:center;color:#c62828">Scan failed</div>';
});}
function showMessage(tab,type,text){
var msg=document.getElementById(tab+'-message');
msg.className='message show '+type;
msg.textContent=text;
setTimeout(function(){msg.classList.remove('show')},5000);}
function saveWiFi(){
var ssid=document.getElementById('ssid').value;
var password=document.getElementById('password').value;
if(!ssid||!password){
showMessage('wifi','error','Please enter both SSID and password');
return;}
var formData=new FormData();
formData.append('ssid',ssid);
formData.append('password',password);
fetch('/api/wifi',{method:'POST',body:formData})
.then(function(r){return r.json()})
.then(function(data){
if(data.success){showMessage('wifi','success',data.message)}
else{showMessage('wifi','error',data.message)}
}).catch(function(err){
showMessage('wifi','error','Failed to save WiFi settings');
});}
function saveMqtt(){
var protocol=document.getElementById('mqttProtocol').value;
var host=document.getElementById('mqttHost').value;
var port=document.getElementById('mqttPort').value;
var username=document.getElementById('mqttUsername').value;
var password=document.getElementById('mqttPassword').value;
if(!host||!port){
showMessage('mqtt','error','Please enter host and port');
return;}
var formData=new FormData();
formData.append('protocol',protocol);
formData.append('host',host);
formData.append('port',port);
formData.append('username',username);
formData.append('password',password);
fetch('/api/mqtt',{method:'POST',body:formData})
.then(function(r){return r.json()})
.then(function(data){
if(data.success){showMessage('mqtt','success',data.message)}
else{showMessage('mqtt','error',data.message)}
}).catch(function(err){
showMessage('mqtt','error','Failed to save MQTT settings');
});}
function testMqtt(){
var protocol=document.getElementById('mqttProtocol').value;
var host=document.getElementById('mqttHost').value;
var port=document.getElementById('mqttPort').value;
var username=document.getElementById('mqttUsername').value;
var password=document.getElementById('mqttPassword').value;
if(!host||!port){
showMessage('mqtt','error','Please enter host and port');
return;}
showMessage('mqtt','success','Testing MQTT connection... please wait');
var formData=new FormData();
formData.append('protocol',protocol);
formData.append('host',host);
formData.append('port',port);
formData.append('username',username);
formData.append('password',password);
fetch('/api/mqtt-test',{method:'POST',body:formData})
.then(function(r){return r.json()})
.then(function(data){
if(data.success){showMessage('mqtt','success',data.message)}
else{showMessage('mqtt','error',data.message)}
}).catch(function(err){
showMessage('mqtt','error','MQTT test failed');
});}
</script></body></html>
)=====";
