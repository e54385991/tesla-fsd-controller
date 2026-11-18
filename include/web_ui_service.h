#pragma once

const char SERVICE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>维护模式 · Tesla Controller</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#0b1120;color:#e2e8f0;font:16px/1.65 -apple-system,system-ui,"PingFang SC",sans-serif;padding:24px 16px}
main{max-width:720px;margin:auto}a{color:#38bdf8}h1{font-size:28px;margin:22px 0 4px}h2{font-size:18px;margin:0 0 12px}p{margin:10px 0}.muted{color:#94a3b8;font-size:14px}.card{background:#131d32;border:1px solid #243149;border-radius:16px;padding:22px;margin:18px 0}.notice{border-color:#785623;background:#261e15}.notice strong{color:#fbbf24}.row{display:flex;justify-content:space-between;gap:20px;padding:10px 0;border-bottom:1px solid #243149}.row:last-child{border:0}.row span:last-child{text-align:right;color:#94a3b8}.actions{display:flex;gap:12px;flex-wrap:wrap;margin-top:16px}button{font:inherit;font-weight:600;border:1px solid #334155;border-radius:10px;background:#1e293b;color:#e2e8f0;padding:12px 20px;cursor:pointer;min-height:48px}button.primary{background:#0369a1;border-color:#0369a1}button:disabled{opacity:.45;cursor:not-allowed}button:focus-visible,a:focus-visible,input:focus-visible{outline:3px solid #38bdf8;outline-offset:3px}button:disabled.primary{background:#334155;border-color:#334155}.msg{min-height:26px;color:#fbbf24;margin-top:12px;white-space:pre-line}ol{padding-left:24px}li{margin:10px 0}code{color:#7dd3fc}.overlay{position:fixed;inset:0;background:#000b;display:flex;align-items:center;justify-content:center;padding:16px;z-index:10}.overlay[hidden]{display:none}.dialog{width:100%;max-width:500px;max-height:90vh;overflow:auto;background:#131d32;border:1px solid #475569;border-radius:16px;padding:24px}input[type=password]{width:100%;padding:12px;border-radius:8px;border:1px solid #475569;background:#0b1120;color:#fff;font:inherit}.check{display:flex;gap:10px;align-items:flex-start}.check input{margin-top:7px}details summary{cursor:pointer;font-weight:600}#connection{font-size:14px}footer{margin:24px 0;color:#64748b;font-size:13px}
@media(min-width:1000px){body{font-size:20px}.muted,#connection{font-size:16px}h1{font-size:34px}button{min-height:56px}}
</style>
</head>
<body><main>
<a href="/" id="back">← 返回控制面板</a>
<h1>维护模式</h1>
<p class="muted">Service Mode · Model X Raven</p>
<section class="card notice" aria-labelledby="availabilityTitle">
  <h2 id="availabilityTitle">自动进入尚未开放</h2>
  <p><strong>当前可观察挡位和车速；自动进入协议仍未验证。</strong>实时数据按公开的旧款 S/X 定义解析，需与仪表核对。本页不会发送进入或退出维护模式的 CAN 指令。</p>
  <p class="muted">目标为用户提供的 2019 Model X Raven、2026.2；此信息不代表已自动识别车辆或已验证兼容。</p>
</section>
<section class="card" aria-labelledby="statusTitle">
  <h2 id="statusTitle">连接与执行条件</h2>
  <div class="row"><span>目标车辆</span><span>2019 Model X Raven</span></div>
  <div class="row"><span>目标软件</span><span>2026.2（含补丁版本）</span></div>
  <div class="row"><span>设备状态</span><span id="device">待检查</span></div>
  <div class="row"><span>车辆维护模式</span><span>未知</span></div>
  <div class="row"><span>当前挡位</span><span id="gear">未知</span></div>
  <div class="row"><span>当前车速</span><span id="speed">未知</span></div>
  <div class="row"><span>P 挡 / 静止状态</span><span id="parkedState">未知</span></div>
  <div class="row"><span>车辆信号</span><span id="vehicleSignal">待检查</span></div>
  <p class="muted">挡位与车速为只读观察值，不代表已验证 Raven 的操作条件，也不会开启自动操作。</p>
  <details><summary>信号诊断</summary><p id="signalDetails" class="muted">等待读取…</p></details>
  <p id="connection" role="status" aria-live="polite">正在读取设备能力…</p>
  <div class="actions">
    <button class="primary" id="enter" disabled>进入维护模式</button>
    <button id="exit" disabled>退出维护模式</button>
    <button id="refresh">重新检查</button>
  </div>
  <p class="muted">自动操作不可用。下方提供车机上的进入与退出步骤。</p>
  <div class="msg" id="result" role="status" aria-live="polite"></div>
</section>
<section class="card">
  <h2>通过车机进入</h2>
  <ol>
    <li>将车辆停稳并挂入 P 挡。</li>
    <li>打开车机「控制 → 软件」。</li>
    <li>长按大号「MODEL」字样约 2 秒后松开。</li>
    <li>输入 <code>service</code>，点击确定，并按车机提示完成确认。</li>
    <li>以车机出现「SERVICE MODE」标识或红色边框为准。</li>
  </ol>
  <details><summary>如何退出维护模式</summary><p>在车机打开「控制 → 维护模式」，点击维护模式菜单顶部的「退出维护模式」，确认红色标识或边框消失。</p></details>
  <p class="muted"><a href="https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-0DC534D7-A4B8-417F-8E42-DF1560E970C2.html" target="_blank" rel="noopener noreferrer">Tesla 官方 Model X 操作说明 ↗</a></p>
</section>
<section class="card">
  <h2>通过 Tesla Toolbox 操作</h2>
  <p>如已具备 Toolbox 3 的车辆诊断连接及权限，可打开官方例程，按官方流程解锁网关并运行。</p>
  <p><a href="https://toolbox.teslamotors.com/?query=PROC_ICE_X_ENTER-SERVICE-MODE&amp;searchType=actions" target="_blank" rel="noopener noreferrer">打开进入维护模式例程 ↗</a></p>
  <p><a href="https://toolbox.teslamotors.com/?query=PROC_ICE_X_EXIT-SERVICE-MODE&amp;searchType=actions" target="_blank" rel="noopener noreferrer">打开退出维护模式例程 ↗</a></p>
  <p class="muted">链接只打开 Toolbox，不会执行例程。连接 ESP32 的 Wi-Fi 不等于建立了车辆诊断连接。</p>
</section>
<footer>本页针对普通 Service Mode。Service Mode Plus 的诊断连接需另行支持。</footer>
</main>
<div class="overlay" id="confirmOverlay" hidden>
  <section class="dialog" role="dialog" aria-modal="true" aria-labelledby="confirmTitle">
    <h2 id="confirmTitle">确认进入维护模式</h2>
    <p>请确认车辆为 2019 Model X Raven，软件为 2026.2 系列，并已停稳、挂入 P 挡。</p>
    <p class="muted">确认后由设备重新检查执行条件；未验证的条件会阻止操作。</p>
    <label class="check"><input type="checkbox" id="parked"><span>我已核实车型、软件版本，车辆处于 P 挡且静止。</span></label>
    <div class="actions"><button class="primary" id="confirmAction" disabled>确认执行</button><button id="cancelAction">取消</button></div>
  </section>
</div>
<div class="overlay" id="pinOverlay" hidden>
  <form class="dialog" id="pinForm" role="dialog" aria-modal="true" aria-labelledby="pinTitle">
    <h2 id="pinTitle">输入控制面板 PIN</h2>
    <label for="pin">PIN</label><input id="pin" type="password" maxlength="16" autocomplete="current-password" required>
    <div class="msg" id="pinError" role="alert"></div>
    <div class="actions"><button class="primary" id="login" type="submit">解锁</button><button id="cancelPin" type="button">取消</button></div>
  </form>
</div>
<script>
'use strict';
var token='';
try{token=sessionStorage.getItem('fsd_tok')||'';}catch(e){}
var capability=null, pendingAction='', busy=false, loading=false, returnFocus=null;
var $=function(id){return document.getElementById(id);};
if(new URLSearchParams(location.search).get('from')==='car') $('back').href='/car';
function authQuery(){return token?'?token='+encodeURIComponent(token):'';}
async function serviceRequest(url,options,text){
  var controller=new AbortController(),timer=setTimeout(function(){controller.abort();},8000);
  try{
    var response=await fetch(url,Object.assign({},options,{signal:controller.signal}));
    return {status:response.status,ok:response.ok,data:await(text?response.text():response.json())};
  }finally{clearTimeout(timer);}
}
function disableActions(){ $('enter').disabled=true; $('exit').disabled=true; }
function clearVehicleState(message){
  $('gear').textContent='未知';$('speed').textContent='未知';$('parkedState').textContent='未知';
  $('vehicleSignal').textContent=message;$('signalDetails').textContent='无可用实时数据。';
}
function renderVehicleState(data){
  var reasons={no_signal:'尚未收到挡位信号',can_unavailable:'CAN 不可用',
    can_session_changed:'CAN 已恢复，等待新信号',stale_signal:'信号已超时',
    unsupported_frame_layout:'报文格式与旧款 S/X 不匹配',checksum_mismatch:'报文校验失败',
    invalid_gear:'挡位值无效',invalid_speed:'车速值无效',observed:'已收到实时数据（待实车核验）'};
  clearVehicleState(reasons[data.signalStatus]||'无法确认车辆信号');
  if(data.signalSource!=='legacy_sx_di_torque2')return;
  var age=Number.isInteger(data.signalAgeMs)&&data.signalAgeMs>=0?data.signalAgeMs+' ms':'未知';
  $('signalDetails').textContent='旧款 S/X · 0x118 · DLC '+data.frameDlc+' · 数据年龄 '+age+
    '\n原始报文：'+(data.frameHex||'未收到')+
    (data.signalStatus==='no_signal'?'\n当前 CAN 总线未观察到 0x118，需核对接入总线。':'');
  if(data.signalFresh!==true||!['observed','invalid_gear','invalid_speed'].includes(data.signalStatus))return;
  var gearValid=typeof data.gear==='string'&&/^[PRND]$/.test(data.gear);
  var speedValid=typeof data.speedKph==='number'&&Number.isFinite(data.speedKph);
  if(gearValid)$('gear').textContent=data.gear;
  if(speedValid)$('speed').textContent=data.speedKph.toFixed(2)+' km/h';
  if(gearValid&&speedValid)$('parkedState').textContent=
    data.gear==='P'&&Math.abs(data.speedKph)<=0.1?'观察到 P 挡且静止':'未同时满足 P 挡与静止';
}
function showPin(){returnFocus=document.activeElement;$('pinOverlay').hidden=false;$('pin').focus();}
function closePin(){ $('pinOverlay').hidden=true;$('pin').value='';if(returnFocus)returnFocus.focus(); }
function closeConfirm(){
  $('confirmOverlay').hidden=true;pendingAction='';$('parked').checked=false;$('confirmAction').disabled=true;
  if(returnFocus)returnFocus.focus();
}
function openConfirm(action){
  if(busy||loading||!capability||capability.protocolVerified!==true||
     capability[action==='enter'?'entrySupported':'exitSupported']!==true)return;
  returnFocus=document.activeElement;pendingAction=action;$('parked').checked=false;$('confirmAction').disabled=true;
  $('confirmTitle').textContent=action==='enter'?'确认进入维护模式':'确认退出维护模式';
  $('confirmOverlay').hidden=false;$('cancelAction').focus();
}
async function refresh(){
  if(loading||busy)return;
  loading=true;capability=null;disableActions();$('refresh').disabled=true;
  clearVehicleState('正在读取…');
  $('device').textContent='待检查';$('connection').textContent='正在读取设备能力…';
  try{
    var response=await serviceRequest('/api/service-mode'+authQuery(),{cache:'no-store'});
    if(response.status===403){$('connection').textContent='请先输入控制面板 PIN。';showPin();return;}
    if(!response.ok)throw new Error('status');
    var data=response.data;
    if(data.protocolVerified!==false||data.entrySupported!==false||data.exitSupported!==false)
      throw new Error('unexpected_capability');
    capability=data;
    renderVehicleState(data);
    $('device').textContent=data.safeMode?'安全模式（CAN 已禁用）':'在线';
    $('connection').textContent=data.safeMode?'设备处于安全模式，自动操作不可用。':'设备已连接；触发协议未验证，自动操作不可用。';
    // This firmware deliberately exposes no enabled transport. Do not enable
    // buttons from generic /api/status speed/gear values or stored UI state.
  }catch(e){clearVehicleState('连接失败');$('connection').textContent='无法确认设备能力，自动操作保持禁用。';}
  finally{loading=false;$('refresh').disabled=false;}
}
async function execute(){
  if(busy||!pendingAction||!$('parked').checked||!capability||capability.protocolVerified!==true)return;
  var action=pendingAction;closeConfirm();busy=true;disableActions();$('refresh').disabled=true;
  $('result').textContent='正在检查执行条件…';
  var body=new URLSearchParams({action:action,vehicle:'model_x_raven',year:'2019',firmware:'2026.2',
    confirm:action==='enter'?'ENTER_SERVICE_MODE':'EXIT_SERVICE_MODE'});
  try{
    var response=await serviceRequest('/api/service-mode'+authQuery(),{method:'POST',body:body});
    if(response.status===403){$('result').textContent='尚未执行，请重新解锁。';showPin();return;}
    var data=response.data;
    // A successful HTTP request is not evidence that the vehicle changed mode.
    if(data.error==='protocol_unverified')$('result').textContent='协议尚未验证，设备未发送控制指令。';
    else if(data.error==='device_safe_mode')$('result').textContent='设备处于安全模式，未执行。';
    else $('result').textContent='设备未确认执行：'+(data.error||'未知结果')+'。请查看车机实际状态。';
  }catch(e){$('result').textContent='请求结果未知。请查看车机实际状态；不会自动重试。';}
  finally{busy=false;$('refresh').disabled=false;capability=null;}
}
$('enter').onclick=function(){openConfirm('enter');};$('exit').onclick=function(){openConfirm('exit');};
$('refresh').onclick=refresh;$('cancelAction').onclick=closeConfirm;$('confirmAction').onclick=execute;
$('parked').onchange=function(){$('confirmAction').disabled=!this.checked;};
$('cancelPin').onclick=closePin;
$('pinForm').onsubmit=async function(event){
  event.preventDefault();if($('login').disabled)return;$('login').disabled=true;$('pinError').textContent='';
  try{
    var response=await serviceRequest('/api/auth',{method:'POST',body:new URLSearchParams({pin:$('pin').value})},true);
    if(!response.ok){$('pinError').textContent='PIN 错误，请重试。';return;}
    var value=response.data;
    if(!/^[A-F0-9]{16}$/.test(value))throw new Error('token');
    token=value;try{sessionStorage.setItem('fsd_tok',token);}catch(e){}closePin();refresh();
  }catch(e){$('pinError').textContent='连接失败，请重试。';}
  finally{$('login').disabled=false;}
};
document.addEventListener('keydown',function(event){
  var overlay=!$('pinOverlay').hidden?$('pinOverlay'):!$('confirmOverlay').hidden?$('confirmOverlay'):null;
  if(!overlay)return;
  if(event.key==='Escape'){event.preventDefault();if(overlay.id==='pinOverlay')closePin();else closeConfirm();}
  if(event.key==='Tab'){
    var items=overlay.querySelectorAll('button:not(:disabled),input:not(:disabled)');
    var first=items[0],last=items[items.length-1];
    if(event.shiftKey&&document.activeElement===first){event.preventDefault();last.focus();}
    else if(!event.shiftKey&&document.activeElement===last){event.preventDefault();first.focus();}
  }
});
refresh();
setInterval(function(){
  if(!document.hidden&&$('pinOverlay').hidden&&$('confirmOverlay').hidden)refresh();
},1000);
document.addEventListener('visibilitychange',function(){if(!document.hidden)refresh();});
</script>
</body></html>
)rawliteral";
