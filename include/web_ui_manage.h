#pragma once
// Lean management page for the FSD-mock-only Wi-Fi bridge build.
// Replaces the legacy CAN/FSD-injection dashboards: AP, upstream Wi-Fi,
// DNS filter, FSD mock summary, OTA and logs only.

static constexpr char MANAGE_HTML[] = R"rawliteral(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FSD Wi-Fi Bridge</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0d1117;color:#e6edf3;font:14px/1.5 "Segoe UI",system-ui,sans-serif}
header{padding:12px 18px;border-bottom:1px solid #30363d;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
h1{font-size:16px;margin:0}.dim{color:#8b949e}
.badge{border:1px solid #30363d;border-radius:999px;padding:2px 9px;font-size:12px;color:#8b949e}
.badge.ok{color:#3fb950;border-color:#238636}.badge.bad{color:#f85149;border-color:#da3633}
.wrap{padding:16px 18px;max-width:1100px;margin:0 auto}
.danger-alert{margin-bottom:12px;padding:10px 12px;border:1px solid #f85149;border-radius:8px;background:#34191d;color:#ffdcd7}
.danger-alert strong{display:block;margin-bottom:2px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-bottom:14px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px 12px}
.k{color:#8b949e;font-size:12px}.v{font-size:18px;font-weight:600;margin-top:3px;word-break:break-all}.v.s{font-size:13px;font-weight:500}
.mrow{display:flex;align-items:center;gap:6px;margin-top:4px;font-size:12px;font-variant-numeric:tabular-nums}
.mbar{flex:1;height:6px;background:#21262d;border-radius:3px;overflow:hidden;min-width:48px}
.mbar i{display:block;height:100%;width:0;background:#3fb950;transition:width .3s}
section{background:#161b22;border:1px solid #30363d;border-radius:8px;margin-bottom:12px}
h2{font-size:12px;margin:0;padding:9px 12px;border-bottom:1px solid #30363d;color:#8b949e;font-weight:600}
.body{padding:10px 12px}
label{display:block;color:#8b949e;font-size:12px;margin:8px 0 3px}
input[type=text],input[type=password],textarea,select{width:100%;background:#0e1419;color:#d7dee3;border:1px solid #2f3c48;border-radius:6px;padding:6px 8px;font:12.5px Consolas,monospace}
textarea{height:120px;resize:vertical}select{font-family:inherit}
button,.btn{display:inline-block;padding:6px 12px;border:1px solid #30363d;border-radius:6px;background:#21262d;color:#e6edf3;text-decoration:none;font-size:12px;cursor:pointer;margin:6px 6px 0 0}
button.primary{background:#1f6feb;border-color:#1f6feb}button.green{background:#238636;border-color:#238636}button.red{background:#da3633;border-color:#da3633}
table{width:100%;border-collapse:collapse;font-size:12.5px}th,td{text-align:left;padding:6px 8px;border-bottom:1px solid #21262d}th{color:#8b949e;font-weight:500}
code{font-family:Consolas,monospace;color:#58a6ff}.msg{font-size:12.5px;color:#8b949e;margin-top:8px}.msg.ok{color:#3fb950}.msg.bad{color:#f85149}
.overlay{position:fixed;inset:0;background:rgba(0,0,0,.72);display:none;align-items:center;justify-content:center;z-index:20}
.box{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:20px;width:min(360px,92vw)}
.box h3{margin:0 0 10px;font-size:16px}.box input{margin:8px 0}
dialog.box{color:#e6edf3}dialog::backdrop{background:rgba(0,0,0,.72)}
#dnsDisableDialog h3,#dnsDisableDialog label{color:#ffaba8;font-weight:600}
#dnsDisableDialog button:disabled{opacity:.55;cursor:default}
@media(max-width:700px){.wrap{padding:10px}header{padding:10px}}
</style>
<body>
<header>
  <h1>FSD Wi-Fi Bridge</h1>
  <span class="badge" id="bUp">上游：--</span>
  <span class="badge fsd-only" id="bFsd">FSD：--</span>
  <span class="dim" id="bHeap"></span>
  <a class="btn fsd-only" href="/fsd">PC 风格 FSD 看板</a>
  <a class="btn fsd-only" href="/client-config">车机客户端配置</a>
  <a class="btn" href="/api/log" target="_blank">系统日志</a>
</header>
<div class="wrap">
  <aside class="danger-alert" id="apInternetWarning" role="status" hidden>
    <strong>AP 设备已禁止访问上游互联网</strong>
    <span id="apInternetWarningDetails"></span>
  </aside>
  <div class="card" style="margin-bottom:12px;border-color:#238636">
    <div class="k">Wi-Fi 桥接版</div>
    <div style="font-size:13px;margin-top:4px">本固件专注 Wi-Fi 热点/上游桥接、DNS 过滤、OTA 与日志（ESP32-S3 版含按需启停的 FSD 虚拟服务器与客户端配置）。CAN 注入功能已完全移除。</div>
  </div>
  <div class="grid">
    <div class="card"><div class="k">AP</div><div class="v s"><span id="apSsid">--</span><br><span class="dim"><span id="apIp">--</span> · 信道 <span id="apCh">--</span> · <span id="apSta">0</span> 客户端</span></div></div>
    <div class="card"><div class="k">上游</div><div class="v s"><span id="upSsid">--</span><br><span class="dim" id="upIp">--</span></div></div>
    <div class="card fsd-only"><div class="k">FSD 载荷</div><div class="v s" id="fsdPayload">--</div></div>
    <div class="card fsd-only"><div class="k">FSD 命中 / DNS 重定向</div><div class="v" id="fsdHits">--</div></div>
    <div class="card"><div class="k">DNS 拦截</div><div class="v s" id="dnsBlkCard">--</div></div>
    <div class="card"><div class="k">CPU（分核心）</div><div class="v s">
      <div class="mrow"><span style="width:42px;color:#8b949e">Core 0</span><span class="mbar"><i id="cpu0Bar"></i></span><span id="cpu0Val" style="width:42px;text-align:right">--</span></div>
      <div class="mrow"><span style="width:42px;color:#8b949e">Core 1</span><span class="mbar"><i id="cpu1Bar"></i></span><span id="cpu1Val" style="width:42px;text-align:right">--</span></div>
      <div class="dim" style="font-size:11px;margin-top:4px" id="cpuMeta">C0: Wi-Fi/TLS · C1: lwIP/DNS</div>
    </div></div>
    <div class="card"><div class="k">内存使用率（RAM）</div><div class="v s">
      <div class="mrow" style="margin-top:2px"><b id="memPct" style="font-size:16px">--</b><span class="mbar"><i id="memBar"></i></span></div>
      <div class="dim" style="font-size:11px;margin-top:4px" id="heapCard">--</div>
    </div></div>
    <div class="card"><div class="k">Reset reason</div><div class="v s" id="resetCard">--</div></div>
  </div>

  <section><h2>Wi-Fi 热点（AP）</h2><div class="body">
    <label>AP 名称（SSID）</label><input type="text" id="apSsidIn" maxlength="32">
    <label>新密码（留空不改，至少 8 位）</label><input type="password" id="apPassIn" maxlength="63">
    <div class="msg bad" id="apWarn" style="display:none">当前使用默认密码 058005800580，建议立即修改。</div>
    <button class="primary" id="apSave">保存并重启</button><div class="msg" id="apMsg"></div>
  </div></section>

  <section><h2>上游 Wi-Fi（可选）</h2><div class="body">
    <label><input type="checkbox" id="upEn"> 启用上游（AP+STA；AP 会跟随上游信道）</label>
    <table><thead><tr><th>SSID</th><th>状态</th><th></th></tr></thead><tbody id="netRows"></tbody></table>
    <label>添加/更新上游</label>
    <input type="text" id="netSsid" maxlength="32" placeholder="SSID">
    <input type="password" id="netPass" maxlength="63" placeholder="密码（开放网络留空）">
    <button id="netAdd">添加</button>
    <button id="netScan">扫描附近 Wi-Fi</button>
    <div class="msg" id="netMsg"></div>
  </div></section>

  <section><h2>DNS 过滤 / 白名单</h2><div class="body">
    <label><input type="checkbox" id="dnsEn" disabled> 启用 DNS 过滤</label>
    <div class="dim" style="font-size:12px;margin-top:2px">当前模式：<b id="dnsMode" style="color:#d29922">--</b></div>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:2px">域名规则适用于本机 DNS（9.9.9.9）。启用 DNS 过滤并保存白名单后，AP 设备才能访问上游。</div>
    <label>白名单（空格/换行分隔）</label>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:-2px">白名单为空时，AP 设备无法访问上游。未命中白名单的 DNS 查询默认拒绝。</div>
    <textarea id="dnsAllow" spellcheck="false"></textarea>
    <label>黑名单</label>
    <textarea id="dnsBlock" spellcheck="false"></textarea>
    <button class="primary" id="dnsSave" disabled>保存</button>
    <button id="dnsResetAllow">白名单恢复默认</button>
    <button id="dnsResetBlock">黑名单恢复默认</button>
    <div style="margin-top:8px;padding:8px 10px;border:1px solid #9e6a03;border-radius:6px;background:#2d2103;color:#e3b341;font-size:12px;line-height:1.6">
      ⚠️ hermes-prd.vn.cloud.tesla.cn 默认在<b>黑名单</b>（无车控版更安全）。仅 <b>Model 3/Y</b> 可能需要开启，以便 FSD 下发时让车机认为在线并请求配置；<b>Model S/X 不开启也会请求配置，且 S/X 请勿开启</b>。勾选需等待 10 秒确认，确认后追加到白名单末尾并<b>临时从黑名单移除</b>，取消勾选后自动恢复黑名单；存在被封禁 FSD 权限的风险。<br>
      ⛔ api-prd.vn.cloud.tesla.cn 默认在黑名单：车机恢复出厂后如 QQ 音乐无法显示扫码登录 / 网易云音乐异常，可先加入白名单（同长度白名单优先）；放行后云端可能改写 FSD 配置，风险自担。音乐恢复后请立刻移回黑名单或断开 Wi-Fi。<br>
      ⛔ mothership.vn.teslamotors.com / mothership.vn.cloud.tesla.cn 默认在外网黑名单；FSD 开启时按 Windows 版劫持到本机 443/4567，关闭后阻断直连。<br>
      ⛔ assistant-api.prd.cnn1.vn.cloud.tesla.cn 默认在外网黑名单（语音助手 / 配置端点），避免云端下发配置。
    </div>
    <label style="margin-top:8px"><input type="checkbox" id="hermesControl" disabled> 启用 hermes 远程车控（默认关闭；仅 3/Y 可能需要，S/X 勿开）</label>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:-2px">取消勾选会把 <code>hermes-prd.vn.cloud.tesla.cn</code> 从白名单<strong>任意位置</strong>移除（不区分大小写/分隔符），并重新加回黑名单。</div>
    <label style="margin-top:12px">IP 层安全策略（lwIP 转发层防护）</label>
    <select id="ipPolicyMode">
      <option value="0">标准（默认：自动阻断 AP→外网 53/853 绕过，无每包 IP 查表开销）</option>
      <option value="1">IP 黑名单拦截（按黑名单解析出的 IP 在转发层丢包，最多 256 条）</option>
      <option value="2">严格 IP 白名单（仅放行经白名单 DNS 解析出的 IP，防硬编码/旧缓存/DoH 绕过）</option>
    </select>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:4px">开启 DNS 过滤后始终自动阻断车机直连外部 <code>UDP/TCP 53</code> 与 <code>DoT 853</code>；「严格 IP 白名单」会记录白名单域名解析出的全部 A 记录（30 分钟滑动续期），可阻止车机直连硬编码或历史缓存的 Tesla IP。上游访问已放行且 FSD 虚拟服务器开启时，443 端口非 <code>/feature_configuration</code> 路径会反代至上游 <code>api-prd</code>。</div>
    <div style="margin-top:14px;padding-top:10px;border-top:1px solid #30363d">
      <div style="display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:8px">
        <div class="dim" style="font-size:12px">DNS 拦截 <b id="dnsBlkTotal" style="color:#f85149">0</b> 次 · IP 拦截 <b id="ipDrops" style="color:#f85149">0</b> 次 · 黑名单缓存 <span id="ipCache">0</span>（峰值 <span id="ipPeak">0</span>） · 白名单 IP <span id="ipAllow">0</span></div>
        <div><button id="dnsBlkClear">清空拦截记录</button><button id="dnsBlkDl">下载 CSV</button></div>
      </div>
      <table style="margin-top:8px">
        <thead><tr><th>屏蔽域名</th><th style="width:64px;text-align:right">次数</th><th style="width:96px;text-align:right">最近</th></tr></thead>
        <tbody id="blkRows"><tr><td colspan="3" class="dim">暂无拦截</td></tr></tbody>
      </table>
      <div class="dim" id="blkNote" style="font-size:11px;margin-top:4px;display:none">仅显示拦截次数最多的前 50 个域名，总数仍会继续累计。</div>
    </div>
    <div class="msg" id="dnsMsg"></div>
  </div></section>

  <section class="fsd-only"><h2>FSD 虚拟服务器</h2><div class="body">
    <label><input type="checkbox" id="fsdEn"> 启动 FSD 虚拟服务器（/feature_configuration，默认关闭）</label>
    <label><input type="checkbox" id="fsdAutoOff"> 下发成功后延迟 2 分钟自动关闭（默认关闭，非阻塞）</label>
    <label>模式</label><select id="fsdMode"><option value="reply">reply（本地回包）</option><option value="observe">observe（只记录并转发）</option></select>
    <div class="dim" id="fsdHealth" style="font-size:12px;margin-top:6px">监听状态：--</div>
    <button class="primary" id="fsdSave">保存 FSD 虚拟服务器设置</button>
    <a class="btn" href="/fsd">PC 风格观察页 / 启动 / 编辑载荷</a>
    <div class="msg" id="fsdMsg"></div>
  </div></section>

  <section><h2>固件 OTA</h2><div class="body">
    <label>上传 .bin（app 镜像）</label><input type="file" id="otaFile" accept=".bin">
    <button class="primary" id="otaBtn">上传并重启</button>
    <button id="otaCheck">检查在线版本</button>
    <button id="otaPull">下载并安装最新版</button>
    <div class="msg" id="otaMsg"></div>
  </div></section>

  <section><h2>系统</h2><div class="body">
    <a class="btn" href="/api/log" target="_blank">查看日志</a>
    <a class="btn" href="/api/log/download">下载日志</a>
    <button id="logClear">清空日志</button>
    <button class="red" id="reboot">重启</button>
    <div class="msg" id="sysMsg"></div>
  </div></section>
</div>

<div class="overlay" id="pinOverlay"><div class="box">
  <h3>输入管理 PIN</h3><div class="dim" style="font-size:12px">设备设置了 PIN。</div>
  <input type="password" id="pinInput" inputmode="numeric" autocomplete="off" placeholder="PIN">
  <button class="primary" id="pinBtn">进入</button><div class="msg bad" id="pinMsg"></div>
</div></div>

<div class="overlay" id="hermesConfirm"><div class="box" style="max-width:540px">
  <h3 style="color:#e3b341">⚠️ 启用 hermes 远程车控？</h3>
  <div style="font-size:12px;line-height:1.8;color:#e3b341">
    • 启用后会把 <code>hermes-prd.vn.cloud.tesla.cn</code> 追加到白名单最后一项，并<b>临时从黑名单移除</b>（取消勾选后恢复黑名单）。<br>
    • <b>仅 Model 3/Y 可能需要开启</b>，让车机认为在线并主动请求配置。<br>
    • <b>Model S/X 不开启也会请求配置；S/X 请勿开启本项</b>。<br>
    • 存在被 Tesla 封禁 FSD 权限的风险。<br>
    • 风险自担；不需要远程车控时请随时取消勾选。
  </div>
  <div class="dim" id="hermesCountdown" style="font-size:12px;margin-top:10px">请阅读以上风险，倒计时结束后才能确认。</div>
  <button class="primary" id="hermesConfirmBtn" disabled>确认启用（10s）</button>
  <button id="hermesCancelBtn">取消</button>
</div></div>

<dialog class="box" id="dnsDisableDialog" aria-labelledby="dnsDisableTitle" aria-describedby="dnsDisableInfo">
  <h3 id="dnsDisableTitle">危险操作 · 禁用 DNS 过滤</h3>
  <div id="dnsDisableInfo">禁用后，连接此热点的所有设备都将失去上游互联网。你仍可通过本机管理页恢复联网。</div>
  <p class="dim" id="dnsDisableCountdown">请等待 10 秒后勾选确认。</p>
  <label><input type="checkbox" id="dnsDisableAck" disabled> [!!危险!!] 确认禁用</label>
  <button type="button" id="dnsDisableCancel" autofocus>取消</button>
  <button type="button" class="red" id="dnsDisableSubmit" disabled>确认禁用</button>
</dialog>

<script>
'use strict';
var token=''; try{token=sessionStorage.getItem('fsd_tok')||'';}catch(e){}
var busy=false;
var refreshQueued=false, bridgeWriteVersion=0;
var pendingAfterPin=null;
var HERMES_DOMAIN='hermes-prd.vn.cloud.tesla.cn';
var dnsSwitch=document.getElementById('dnsEn');
var dnsDisableDialog=document.getElementById('dnsDisableDialog');
var dnsDisableAck=document.getElementById('dnsDisableAck');
var dnsDisableSubmit=document.getElementById('dnsDisableSubmit');
var dnsDisableCountdown=document.getElementById('dnsDisableCountdown');
var dnsEnabled=null, dnsSaving=false, dnsDisableDeadline=0, dnsDisableTimer=null;
var savedDnsRules={dnsAllow:'',dnsBlock:''};
// Rules may be separated by spaces, newlines, commas or semicolons (same as
// the firmware parser). Compare case-insensitively and ignore a trailing dot
// so the hermes entry is recognised no matter where/how it was pasted.
function dnsTokens(list){
  return (list||'').split(/[\s,;]+/).map(function(x){return x.trim();}).filter(Boolean);
}
function isHermesDomain(x){
  return String(x).toLowerCase().replace(/\.+$/,'')===HERMES_DOMAIN;
}
function listHasHermes(list){ return dnsTokens(list).some(isHermesDomain); }
function stripHermes(list){ return dnsTokens(list).filter(function(x){return !isHermesDomain(x);}); }
function q(u){return u+(token?(u.indexOf('?')>=0?'&':'?')+'token='+encodeURIComponent(token):'');}
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function showPin(){closeDnsDisable();document.getElementById('pinOverlay').style.display='flex';setTimeout(function(){document.getElementById('pinInput').focus();},50);}
function setMsg(id,t,ok){var e=document.getElementById(id);e.textContent=t||'';e.className='msg '+(ok===true?'ok':(ok===false?'bad':''));}
function barColor(p){return p>=85?'#f85149':(p>=65?'#d29922':'#3fb950');}
function setBar(id,p){var el=document.getElementById(id);if(!el)return;var v=(p!=null&&isFinite(p))?Math.max(0,Math.min(100,Number(p))):0;el.style.width=v.toFixed(1)+'%';el.style.background=barColor(v);}
function fmtKb(b){return (Number(b||0)/1024).toFixed(1)+' KB';}
async function getText(u){var r=await fetch(q(u),{cache:'no-store'});if(r.status===403){showPin();throw new Error('auth');}if(!r.ok)throw new Error('HTTP '+r.status);return await r.text();}
async function getJson(u){return JSON.parse(await getText(u));}
function syncDnsRule(id,saved){
  var input=document.getElementById(id);
  saved=saved.replace(/\r\n?/g,'\n');
  if(input.value===savedDnsRules[id]) input.value=saved;
  savedDnsRules[id]=saved;
}
function renderDnsStatus(d){
  dnsEnabled=!!d.dnsEnable;
  dnsSwitch.checked=dnsEnabled;
  dnsSwitch.disabled=dnsSaving;
  if(!dnsEnabled)closeDnsDisable();
  syncDnsRule('dnsAllow',d.dnsAllow||'');
  syncDnsRule('dnsBlock',d.dnsBlock||'');
  var hermes=document.getElementById('hermesControl');
  hermes.checked=listHasHermes(d.dnsAllow);
  hermes.disabled=false;
  document.getElementById('dnsSave').disabled=false;

  var reasons=d.apInternetBlockReasons||[], causes=[], actions=[];
  if(reasons.includes('dns_filter_disabled')){
    causes.push('DNS 过滤已关闭');
    actions.push('开启 DNS 过滤');
  }
  if(reasons.includes('dns_allowlist_empty')){
    causes.push('白名单为空');
    actions.push('添加白名单规则或恢复默认白名单');
  }
  var warning=document.getElementById('apInternetWarning');
  var details=document.getElementById('apInternetWarningDetails');
  var warningText=causes.length?causes.join('，')+'。请'+actions.join('，并')+'。':'';
  if(details.textContent!==warningText)details.textContent=warningText;
  warning.hidden=reasons.length===0;

  var ipMode=(d.ipPolicyMode!=null)?Number(d.ipPolicyMode):(d.ipBlockerEnabled?1:0);
  if(!document.getElementById('ipPolicyMode').matches(':focus')) document.getElementById('ipPolicyMode').value=String(ipMode);
  var modeEl=document.getElementById('dnsMode');
  modeEl.textContent=reasons.length?'AP 上游访问已阻断':'默认拒绝：仅放行白名单（'+(d.dnsAllowCount||0)+' 条'+(ipMode===2?' · 严格 IP 白名单':'')+'）';
  modeEl.style.color=reasons.length?'#f85149':'#3fb950';
}
async function refresh(){
  if(document.hidden)return;
  if(busy){refreshQueued=true;return;}
  busy=true;
  var version=bridgeWriteVersion;
  try{
    var d=await getJson('/api/wifi-bridge/status'), hasFsd=!!d.fsdMock, f=d.fsdMock||{};
    if(version!==bridgeWriteVersion)return;
    document.querySelectorAll('.fsd-only').forEach(function(el){el.style.display=hasFsd?'':'none';});
    document.getElementById('bUp').textContent='上游：'+(d.upstreamConnected?(d.upstreamSSID||'已连接'):'未启用');
    document.getElementById('bUp').className='badge '+(d.upstreamConnected?'ok':'');
    var fsdBadge=document.getElementById('bFsd');
    if(f.enabled&&!f.active){fsdBadge.textContent='FSD：未监听（自动恢复中）';fsdBadge.className='badge bad';}
    else if(f.enabled&&f.autoOffPending){fsdBadge.textContent='FSD：已下发 · '+(f.autoOffRemainingSec||0)+'s 后自动关闭';fsdBadge.className='badge ok';}
    else if(f.enabled){fsdBadge.textContent='FSD：开 / '+(f.mode||'--');fsdBadge.className='badge ok';}
    else{fsdBadge.textContent='FSD：关'+(f.autoDisabledCount?'（已自动关闭）':'');fsdBadge.className='badge';}
    var memPct=(d.memUsedPct!=null)?Number(d.memUsedPct):((d.heapTotal>0&&d.heap!=null)?((d.heapTotal-d.heap)*100/d.heapTotal):null);
    var c0=(d.cpu0!=null)?Number(d.cpu0):null, c1=(d.cpu1!=null)?Number(d.cpu1):null;
    document.getElementById('bHeap').textContent=
      (c0!=null&&c1!=null?('CPU '+c0.toFixed(0)+'%/'+c1.toFixed(0)+'% · '):'')+
      (memPct!=null?('RAM '+memPct.toFixed(1)+'% ('+fmtKb(d.heap)+' 剩余)'):((d.heap||0)+' B free'));
    document.getElementById('apSsid').textContent=d.apSSID||'fsd-esp32';
    if(!document.getElementById('apSsidIn').matches(':focus'))
      document.getElementById('apSsidIn').value=d.apSSID||'fsd-esp32';
    document.getElementById('apIp').textContent=d.apIP||'9.9.9.9';
    document.getElementById('apCh').textContent=d.channel==null?'--':d.channel;
    document.getElementById('apSta').textContent=d.apStations==null?'0':d.apStations;
    document.getElementById('apWarn').style.display=d.apPassDefault?'':'none';
    document.getElementById('upSsid').textContent=d.upstreamConnected?(d.upstreamSSID||'--'):'未连接';
    document.getElementById('upIp').textContent=d.upstreamConnected?(d.upstreamIP||''):(d.upstreamStatus||'');
    document.getElementById('upEn').checked=!!d.upstreamEnable;
    document.getElementById('fsdPayload').textContent=(f.bytes||0)+' B / '+(f.md5||'').slice(0,10)+' / '+(f.fields||0)+' 字段';
    document.getElementById('fsdHits').textContent=(f.hits||0)+' / '+(f.dnsRedirects||0);
    document.getElementById('cpu0Val').textContent=c0!=null?(c0.toFixed(1)+'%'):'--';
    document.getElementById('cpu1Val').textContent=c1!=null?(c1.toFixed(1)+'%'):'--';
    setBar('cpu0Bar',c0); setBar('cpu1Bar',c1);
    document.getElementById('cpuMeta').textContent=
      (d.cpuAvg!=null?('均 '+Number(d.cpuAvg).toFixed(1)+'%'):'C0: Wi-Fi/TLS · C1: lwIP/DNS')+
      (d.chipTempC!=null?(' · 温度 '+Number(d.chipTempC).toFixed(1)+'°C'):'');
    document.getElementById('memPct').textContent=memPct!=null?(memPct.toFixed(1)+'%'):'--';
    document.getElementById('memPct').style.color=memPct!=null?barColor(memPct):'#e6edf3';
    setBar('memBar',memPct);
    document.getElementById('heapCard').textContent=
      '剩 '+fmtKb(d.heap)+(d.heapTotal?(' / 总 '+fmtKb(d.heapTotal)):'')+
      ' · 低 '+fmtKb(d.minHeap)+(d.memPeakPct!=null?(' (峰 '+Number(d.memPeakPct).toFixed(1)+'%)'):'');
    document.getElementById('resetCard').textContent=d.resetReason||'--';
    document.getElementById('fsdEn').checked=!!f.enabled;
    document.getElementById('fsdAutoOff').checked=f.autoOffOnHit!==false;
    document.getElementById('fsdMode').value=f.mode==='observe'?'observe':'reply';
    var fh='443:'+(f.fsd443?'监听中':'未监听')+' · 4567:'+(f.fsd4567?'监听中':'未监听')+
      ' · DNS劫持:'+(f.active&&f.enabled?'开':'关')+
      (f.autoOffPending?(' · 倒计时:'+(f.autoOffRemainingSec||0)+'s'):'')+
      (f.recoveries?(' · 自复:'+f.recoveries+'次'):'')+
      (f.autoDisabledCount?(' · 已自动关闭:'+f.autoDisabledCount+'次'):'')+
      (f.lastError?(' · 异常:'+f.lastError):'');
    var fhEl=document.getElementById('fsdHealth');
    fhEl.textContent=fh;
    fhEl.style.color=((f.enabled&&!f.active)||f.lastError)?'#f85149':(f.active&&f.enabled?'#3fb950':'#8b949e');
    var nets=d.upstreamNetworks||[];
    document.getElementById('netRows').innerHTML=nets.map(function(n){
      return '<tr><td>'+esc(n.ssid)+'</td><td>'+(n.connected?'<span style="color:#3fb950">已连接</span>':(n.active?'<span style="color:#d29922">连接中</span>':'<span class="dim">未连接</span>'))+'</td><td><button class="red" data-del="'+esc(n.ssid)+'">删除</button></td></tr>';
    }).join('')||'<tr><td colspan="3" class="dim">无已保存上游</td></tr>';
    document.querySelectorAll('[data-del]').forEach(function(b){b.onclick=function(){delNet(b.getAttribute('data-del'));};});
    renderDnsStatus(d);
    var blk=d.dnsBlockedRecent||[], blkTotal=d.dnsBlockedTotal||0;
    document.getElementById('dnsBlkCard').textContent=blkTotal+' 次 / '+blk.length+' 域名';
    document.getElementById('dnsBlkTotal').textContent=blkTotal;
    document.getElementById('ipDrops').textContent=d.ipBlockDrops||0;
    document.getElementById('ipCache').textContent=d.ipCacheCount||0;
    document.getElementById('ipPeak').textContent=d.ipCachePeak||0;
    document.getElementById('ipAllow').textContent=d.ipAllowedCount||0;
    var rows=document.getElementById('blkRows');
    if(!blk.length){
      rows.innerHTML='<tr><td colspan="3" class="dim">暂无拦截</td></tr>';
    }else{
      rows.innerHTML=blk.map(function(e){
        var ago=Math.max(0,(d.uptime||0)-(e.lastBlockedAt||0));
        var when=ago<60?(ago+' 秒前'):(ago<3600?(Math.floor(ago/60)+' 分钟前'):(Math.floor(ago/3600)+' 小时前'));
        return '<tr><td style="word-break:break-all">'+esc(e.domain)+'</td><td style="text-align:right;font-variant-numeric:tabular-nums">'+(e.count||0)+'</td><td style="text-align:right;color:#8b949e">'+when+'</td></tr>';
      }).join('');
    }
    document.getElementById('blkNote').style.display=blk.length>=50?'':'none';
  }catch(e){}finally{
    busy=false;
    if(refreshQueued){refreshQueued=false;refresh();}
  }
}
async function saveAp(){
  var body=new URLSearchParams();body.set('ssid',document.getElementById('apSsidIn').value.trim());
  var p=document.getElementById('apPassIn').value; if(p)body.set('pass',p);
  var r=await fetch(q('/api/wifi'),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
  setMsg('apMsg',r.ok?'已保存，设备将重启':'保存失败',r.ok); if(r.status===403)showPin();
}
async function setBridgeParams(params){
  var r=await fetch(q('/api/wifi-bridge/set?'+params),{cache:'no-store'});
  if(r.ok)bridgeWriteVersion++;
  return r;
}
async function saveDnsRules(){
  var allow=document.getElementById('dnsAllow').value;
  var block=document.getElementById('dnsBlock').value;
  try{
    var r=await setBridgeParams('dnsAllow='+encodeURIComponent(allow)+'&dnsBlock='+encodeURIComponent(block));
    if(r.ok){
      savedDnsRules.dnsAllow=allow;
      savedDnsRules.dnsBlock=block;
    }
    setMsg('dnsMsg',r.ok?'已保存':'保存失败',r.ok);
    if(r.status===403){
      pendingAfterPin=saveDnsRules;
      showPin();
      return;
    }
  }catch(e){setMsg('dnsMsg','保存失败',false);}
  refresh();
}
async function saveUpstream(enabled){
  try{
    var r=await setBridgeParams('upstreamEnable='+(enabled?1:0));
    setMsg('netMsg',r.ok?'已保存':'保存失败',r.ok);
    if(r.status===403){
      pendingAfterPin=function(){saveUpstream(enabled);};
      showPin();
      return;
    }
  }catch(e){setMsg('netMsg','保存失败',false);}
  refresh();
}
async function saveDnsEnabled(enabled){
  if(dnsSaving)return;
  dnsSaving=true;
  dnsSwitch.disabled=true;
  try{
    var r=await setBridgeParams('dnsEnable='+(enabled?1:0));
    if(r.ok){
      dnsEnabled=enabled;
      dnsSwitch.checked=enabled;
      setMsg('dnsMsg',enabled?'DNS 过滤已开启':'DNS 过滤已禁用',true);
    }else if(r.status===403){
      setMsg('dnsMsg','请重新验证 PIN 后再操作。',false);
      showPin();
      return;
    }else{
      setMsg('dnsMsg','保存失败',false);
    }
  }catch(e){setMsg('dnsMsg','保存失败',false);}
  finally{
    dnsSaving=false;
    dnsSwitch.disabled=dnsEnabled===null;
  }
  refresh();
}
function closeDnsDisable(){
  if(!dnsDisableDialog.open)return;
  clearInterval(dnsDisableTimer);
  dnsDisableTimer=null;
  dnsDisableDeadline=0;
  dnsDisableAck.checked=false;
  dnsDisableAck.disabled=true;
  dnsDisableSubmit.disabled=true;
  dnsDisableDialog.close();
}
function updateDnsDisableCountdown(){
  var left=Math.max(0,Math.ceil((dnsDisableDeadline-performance.now())/1000));
  dnsDisableAck.disabled=left>0;
  dnsDisableSubmit.disabled=left>0||!dnsDisableAck.checked;
  var text=left?'请等待 '+left+' 秒后勾选确认。':'请勾选确认后提交。';
  if(dnsDisableCountdown.textContent!==text)dnsDisableCountdown.textContent=text;
  if(!left){clearInterval(dnsDisableTimer);dnsDisableTimer=null;}
}
function toggleDns(){
  var enabled=dnsSwitch.checked;
  dnsSwitch.checked=!!dnsEnabled;
  if(dnsEnabled===null||dnsSaving||enabled===dnsEnabled||dnsDisableDialog.open)return;
  if(enabled){
    saveDnsEnabled(true);
    return;
  }
  dnsDisableDialog.showModal();
  dnsDisableDeadline=performance.now()+10000;
  updateDnsDisableCountdown();
  dnsDisableTimer=setInterval(updateDnsDisableCountdown,250);
}
var hermesTimer=null;
function closeHermesConfirm(){
  if(hermesTimer){clearInterval(hermesTimer);hermesTimer=null;}
  document.getElementById('hermesConfirm').style.display='none';
}
function showHermesConfirm(){
  var overlay=document.getElementById('hermesConfirm');
  var btn=document.getElementById('hermesConfirmBtn');
  var cd=document.getElementById('hermesCountdown');
  var left=10;
  btn.disabled=true; btn.textContent='确认启用（'+left+'s）';
  cd.textContent='请阅读以上风险，倒计时结束后才能确认。';
  overlay.style.display='flex';
  if(hermesTimer) clearInterval(hermesTimer);
  hermesTimer=setInterval(function(){
    left--;
    if(left>0){
      btn.textContent='确认启用（'+left+'s）';
    }else{
      clearInterval(hermesTimer); hermesTimer=null;
      btn.disabled=false; btn.textContent='确认启用';
      cd.textContent='倒计时结束，可以确认启用了。';
    }
  },1000);
}
function applyHermesBlock(want){
  // hermes sits in the default blacklist. Enabling vehicle control temporarily
  // removes it; disabling puts it back so direct access stays blocked.
  var tb=document.getElementById('dnsBlock');
  var parts=stripHermes(tb.value);
  if(!want) parts.push(HERMES_DOMAIN);
  tb.value=parts.join(' ');
}
function applyHermes(want){
  var ta=document.getElementById('dnsAllow');
  // Removing always strips every occurrence wherever it sits (head/middle/tail,
  // any separator or letter case); enabling always appends exactly one copy.
  var parts=stripHermes(ta.value);
  if(want) parts.push(HERMES_DOMAIN);
  ta.value=parts.join(' ');
  applyHermesBlock(want);
}
async function toggleHermes(){
  var box=document.getElementById('hermesControl');
  if(!box.checked){
    // Disabling is safe and immediate: just remove it from the tail.
    applyHermes(false);
    await saveDnsRules();
    return;
  }
  // Never enable on a single click: revert and require the 10s confirmation.
  box.checked=false;
  showHermesConfirm();
}
async function resetDns(which){
  var id=which==='allow'?'dnsAllow':'dnsBlock';
  var before=document.getElementById(id).value;
  var r=await setBridgeParams('dnsReset='+which);
  setMsg('dnsMsg',r.ok?(which==='allow'?'白名单已恢复默认':'黑名单已恢复默认'):'恢复默认失败',r.ok);
  if(r.status===403)showPin();
  if(r.ok){
    savedDnsRules[id]=before;
    refresh();
  }
}
async function saveIpPolicy(){
  var m=document.getElementById('ipPolicyMode').value;
  var r=await setBridgeParams('ipPolicyMode='+encodeURIComponent(m));
  var labels={'0':'标准模式（外部 53/853 拦截）','1':'IP 黑名单拦截已开启','2':'严格 IP 白名单已开启'};
  setMsg('dnsMsg',r.ok?(labels[m]||'已保存'):'保存失败',r.ok);
  if(r.status===403)showPin();
  refresh();
}
async function dnsBlkClear(){
  if(!confirm('清空 DNS / IP 拦截统计？'))return;
  var r=await fetch(q('/api/wifi-bridge/blocked-clear'));
  setMsg('dnsMsg',r.ok?'拦截记录已清空':'清空失败',r.ok);
  if(r.status===403)showPin();
  refresh();
}
function dnsBlkDl(){
  var a=document.createElement('a');
  a.href=q('/api/wifi-bridge/blocked-download');
  a.download='dns_blocked.csv';
  document.body.appendChild(a);a.click();document.body.removeChild(a);
}
async function addNet(){
  var ssid=document.getElementById('netSsid').value.trim(), pass=document.getElementById('netPass').value;
  var u='/api/wifi-bridge/add?ssid='+encodeURIComponent(ssid)+(pass?'&pass='+encodeURIComponent(pass):'');
  var r=await fetch(q(u)); setMsg('netMsg',r.ok?'已添加':'添加失败',r.ok); if(r.ok){document.getElementById('netSsid').value='';document.getElementById('netPass').value='';} refresh();
}
async function delNet(ssid){
  if(!confirm('删除上游 '+ssid+'？'))return;
  var r=await fetch(q('/api/wifi-bridge/delete?ssid='+encodeURIComponent(ssid))); setMsg('netMsg',r.ok?'已删除':'删除失败',r.ok); refresh();
}
async function scan(){
  setMsg('netMsg','扫描中…',null);
  await fetch(q('/api/scan')); 
  for(var i=0;i<8;i++){
    await new Promise(function(r){setTimeout(r,1200);});
    var t=await getText('/api/scan/result');
    if(t.indexOf('scanning')<0){ var arr=JSON.parse(t); setMsg('netMsg',arr.length?('找到 '+arr.length+' 个网络，点击填入：'):'未找到网络',null);
      if(arr.length){ var box=document.getElementById('netRows'); var extra=arr.map(function(x){return '<button data-pick="'+esc(x.ssid)+'">'+esc(x.ssid)+' ('+x.rssi+'dBm)</button>';}).join(''); var old=document.getElementById('scanPick'); if(!old){old=document.createElement('div');old.id='scanPick';box.parentNode.parentNode.appendChild(old);} old.innerHTML=extra; old.querySelectorAll('[data-pick]').forEach(function(b){b.onclick=function(){document.getElementById('netSsid').value=b.getAttribute('data-pick');};}); }
      return;
    }
  }
  setMsg('netMsg','扫描超时',false);
}
async function saveFsd(){
  var en=document.getElementById('fsdEn').checked?1:0;
  var ao=document.getElementById('fsdAutoOff').checked?1:0;
  var u='/api/fsd-mock/set?enabled='+en+'&autoOff='+ao+'&mode='+encodeURIComponent(document.getElementById('fsdMode').value);
  var r=await fetch(q(u));
  setMsg('fsdMsg',r.ok?(en?'已保存，正在启动 FSD 虚拟服务器…':'已停止 FSD 虚拟服务器'):'保存失败',r.ok);
  if(r.status===403)showPin();
  setTimeout(refresh,450);
}
async function otaUpload(){
  var f=document.getElementById('otaFile').files[0]; if(!f){setMsg('otaMsg','请选择 .bin',false);return;}
  var fd=new FormData();fd.append('firmware',f);setMsg('otaMsg','上传中…',null);
  try{var r=await fetch(q('/api/ota'),{method:'POST',body:fd});var t=await r.text();setMsg('otaMsg',t==='OK'?'上传成功，设备重启中…':'上传失败：'+t,r.ok&&t==='OK');}catch(e){setMsg('otaMsg','上传中断：'+e,false);}
}
async function otaCheck(){var r=await fetch(q('/api/ota/check'),{method:'POST'});setMsg('otaMsg',r.ok?'检查已开始，请稍后点“下载并安装最新版”':'检查失败（需先连接上游 Wi-Fi）',r.ok);}
async function otaPull(){var r=await fetch(q('/api/ota/pull'),{method:'POST'});setMsg('otaMsg',r.ok?'开始下载，完成后自动重启':'下载失败（请先检查更新或连接上游 Wi-Fi）',r.ok);}
async function logClear(){var r=await fetch(q('/api/log/clear'),{method:'POST'});setMsg('sysMsg',r.ok?'日志已清空':'失败',r.ok);}
async function reboot(){if(!confirm('重启设备？'))return;await fetch(q('/api/reboot'));setMsg('sysMsg','重启中…',null);}
document.getElementById('apSave').onclick=saveAp;
document.getElementById('netAdd').onclick=addNet;
document.getElementById('netScan').onclick=scan;
document.getElementById('dnsSave').onclick=saveDnsRules;
document.getElementById('dnsResetAllow').onclick=function(){resetDns('allow');};
document.getElementById('dnsResetBlock').onclick=function(){resetDns('block');};
document.getElementById('hermesControl').onchange=toggleHermes;
document.getElementById('hermesCancelBtn').onclick=function(){
  closeHermesConfirm();
  document.getElementById('hermesControl').checked=false;
};
document.getElementById('hermesConfirmBtn').onclick=async function(){
  closeHermesConfirm();
  document.getElementById('hermesControl').checked=true;
  applyHermes(true);
  await saveDnsRules();
};
document.getElementById('dnsBlkClear').onclick=dnsBlkClear;
document.getElementById('dnsBlkDl').onclick=dnsBlkDl;
document.getElementById('fsdSave').onclick=saveFsd;
document.getElementById('fsdEn').onchange=saveFsd;
document.getElementById('fsdAutoOff').onchange=saveFsd;
document.getElementById('fsdMode').onchange=saveFsd;
document.getElementById('upEn').onchange=function(){saveUpstream(this.checked);};
dnsSwitch.onchange=toggleDns;
dnsDisableAck.onchange=updateDnsDisableCountdown;
document.getElementById('dnsDisableCancel').onclick=closeDnsDisable;
dnsDisableDialog.addEventListener('cancel',function(e){e.preventDefault();closeDnsDisable();});
dnsDisableSubmit.onclick=function(){
  if(!dnsDisableDialog.open||dnsSaving||dnsEnabled!==true||performance.now()<dnsDisableDeadline||!dnsDisableAck.checked)return;
  closeDnsDisable();
  saveDnsEnabled(false);
};
document.getElementById('ipPolicyMode').onchange=saveIpPolicy;
document.getElementById('otaBtn').onclick=otaUpload;
document.getElementById('otaCheck').onclick=otaCheck;
document.getElementById('otaPull').onclick=otaPull;
document.getElementById('logClear').onclick=logClear;
document.getElementById('reboot').onclick=reboot;
document.getElementById('pinBtn').onclick=async function(){
  var r=await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pin='+encodeURIComponent(document.getElementById('pinInput').value)});
  if(r.ok){token=(await r.text()).trim();try{sessionStorage.setItem('fsd_tok',token);}catch(e){}document.getElementById('pinOverlay').style.display='none';
    if(pendingAfterPin){var fn=pendingAfterPin;pendingAfterPin=null;fn();}else refresh();}
  else setMsg('pinMsg',r.status===429?'尝试过多，请 10 秒后再试':'PIN 错误',false);
};
document.addEventListener('visibilitychange',function(){if(!document.hidden)refresh();});
refresh();setInterval(refresh,5000);
</script>
</body></html>)rawliteral";
