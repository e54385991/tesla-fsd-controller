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
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-bottom:14px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px 12px}
.k{color:#8b949e;font-size:12px}.v{font-size:18px;font-weight:600;margin-top:3px;word-break:break-all}.v.s{font-size:13px;font-weight:500}
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
    <div class="card"><div class="k">Heap / Min</div><div class="v s" id="heapCard">--</div></div>
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
    <label><input type="checkbox" id="dnsEn"> 启用 DNS 过滤</label>
    <div class="dim" style="font-size:12px;margin-top:2px">当前模式：<b id="dnsMode" style="color:#d29922">--</b></div>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:2px">仅对经过本机 DNS（9.9.9.9）的查询生效。车机/手机开启私人 DNS（DoT/DoH）或使用硬编码 IP 会绕过本过滤。</div>
    <label>白名单（空格/换行分隔）</label>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:-2px">白名单非空时启用「默认拒绝」：未列出的域名一律拦截；白名单为空时只按黑名单拦截。</div>
    <textarea id="dnsAllow" spellcheck="false"></textarea>
    <label>黑名单</label>
    <textarea id="dnsBlock" spellcheck="false"></textarea>
    <button class="primary" id="dnsSave">保存</button>
    <button id="dnsResetAllow">白名单恢复默认</button>
    <button id="dnsResetBlock">黑名单恢复默认</button>
    <div style="margin-top:8px;padding:8px 10px;border:1px solid #9e6a03;border-radius:6px;background:#2d2103;color:#e3b341;font-size:12px;line-height:1.6">
      ⚠️ hermes-prd.vn.cloud.tesla.cn 默认不放行（无车控版更安全）；勾选「启用 hermes 远程车控」需等待 10 秒确认，确认后追加到白名单末尾。FSD 下发时可能需要保持开启让车机认为在线；用过 CAN 模块或改过 FSD 的存在被封禁 FSD 权限的风险。<br>
      ⛔ api-prd.vn.cloud.tesla.cn 默认在黑名单：车机恢复出厂后如 QQ 音乐无法显示扫码登录 / 网易云音乐异常，可先加入白名单（同长度白名单优先）；放行后云端可能改写 FSD 配置，风险自担。音乐恢复后请立刻移回黑名单或断开 Wi-Fi。<br>
      ⛔ mothership.vn.teslamotors.com / mothership.vn.cloud.tesla.cn 默认在外网黑名单；FSD 开启时按 Windows 版劫持到本机 443/4567，关闭后阻断直连。<br>
      ⛔ assistant-api.prd.cnn1.vn.cloud.tesla.cn 默认在外网黑名单（语音助手 / 配置端点），避免云端下发配置。
    </div>
    <label style="margin-top:8px"><input type="checkbox" id="hermesControl"> 启用 hermes 远程车控（追加到白名单最后，谨慎）</label>
    <label style="margin-top:12px"><input type="checkbox" id="ipBlkEn"> IP 层拦截（按黑名单解析出的 IP 在转发层丢包）</label>
    <div class="dim" style="font-size:11px;line-height:1.6;margin-top:-2px">默认关闭。关闭可省去每包 lwIP 钩子开销；开启后才会拦截已拿到 IP、绕过 DNS 继续直连的黑名单流量（最多缓存 256 个 IP）。</div>
    <div style="margin-top:14px;padding-top:10px;border-top:1px solid #30363d">
      <div style="display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:8px">
        <div class="dim" style="font-size:12px">DNS 拦截 <b id="dnsBlkTotal" style="color:#f85149">0</b> 次 · IP 拦截 <b id="ipDrops" style="color:#f85149">0</b> 次 · IP 缓存 <span id="ipCache">0</span>（峰值 <span id="ipPeak">0</span>）</div>
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
    <label><input type="checkbox" id="fsdAutoOff"> 下发成功后延迟 2 分钟自动关闭（默认开启，非阻塞）</label>
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
    • 启用后会把 <code>hermes-prd.vn.cloud.tesla.cn</code> 追加到白名单最后一项。<br>
    • FSD 下发时可能需要保持开启，让车机认为在线并主动请求配置。<br>
    • 如果你之前用过 CAN 模块，或开通 / 改动过 FSD，存在被 Tesla 封禁 FSD 权限的风险。<br>
    • 风险自担；不需要远程车控时请随时取消勾选。
  </div>
  <div class="dim" id="hermesCountdown" style="font-size:12px;margin-top:10px">请阅读以上风险，倒计时结束后才能确认。</div>
  <button class="primary" id="hermesConfirmBtn" disabled>确认启用（10s）</button>
  <button id="hermesCancelBtn">取消</button>
</div></div>

<script>
'use strict';
var token=''; try{token=sessionStorage.getItem('fsd_tok')||'';}catch(e){}
var busy=false;
var pendingAfterPin=null;
function q(u){return u+(token?(u.indexOf('?')>=0?'&':'?')+'token='+encodeURIComponent(token):'');}
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function showPin(){document.getElementById('pinOverlay').style.display='flex';setTimeout(function(){document.getElementById('pinInput').focus();},50);}
function setMsg(id,t,ok){var e=document.getElementById(id);e.textContent=t||'';e.className='msg '+(ok===true?'ok':(ok===false?'bad':''));}
async function getText(u){var r=await fetch(q(u),{cache:'no-store'});if(r.status===403){showPin();throw new Error('auth');}if(!r.ok)throw new Error('HTTP '+r.status);return await r.text();}
async function getJson(u){return JSON.parse(await getText(u));}
async function refresh(){
  if(document.hidden||busy)return;busy=true;
  try{
    var d=await getJson('/api/wifi-bridge/status'), hasFsd=!!d.fsdMock, f=d.fsdMock||{};
    document.querySelectorAll('.fsd-only').forEach(function(el){el.style.display=hasFsd?'':'none';});
    document.getElementById('bUp').textContent='上游：'+(d.upstreamConnected?(d.upstreamSSID||'已连接'):'未启用');
    document.getElementById('bUp').className='badge '+(d.upstreamConnected?'ok':'');
    var fsdBadge=document.getElementById('bFsd');
    if(f.enabled&&!f.active){fsdBadge.textContent='FSD：未监听（自动恢复中）';fsdBadge.className='badge bad';}
    else if(f.enabled&&f.autoOffPending){fsdBadge.textContent='FSD：已下发 · '+(f.autoOffRemainingSec||0)+'s 后自动关闭';fsdBadge.className='badge ok';}
    else if(f.enabled){fsdBadge.textContent='FSD：开 / '+(f.mode||'--');fsdBadge.className='badge ok';}
    else{fsdBadge.textContent='FSD：关'+(f.autoDisabledCount?'（已自动关闭）':'');fsdBadge.className='badge';}
    document.getElementById('bHeap').textContent=(d.heap||0)+' B free';
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
    document.getElementById('heapCard').textContent=(d.heap||0)+' / '+(d.minHeap||0)+' B';
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
    if(!document.getElementById('dnsAllow').matches(':focus')) document.getElementById('dnsAllow').value=d.dnsAllow||'';
    if(!document.getElementById('dnsBlock').matches(':focus')) document.getElementById('dnsBlock').value=d.dnsBlock||'';
    document.getElementById('hermesControl').checked =
      (d.dnsAllow||'').split(/\s+/).indexOf('hermes-prd.vn.cloud.tesla.cn') >= 0;
    document.getElementById('dnsEn').checked=!!d.dnsEnable;
    document.getElementById('ipBlkEn').checked=!!d.ipBlockerEnabled;
    var allowCount=d.dnsAllowCount||0, modeEl=document.getElementById('dnsMode');
    if(!d.dnsEnable){modeEl.textContent='过滤已关闭（全部放行）';modeEl.style.color='#8b949e';}
    else if(allowCount>0){modeEl.textContent='默认拒绝：仅放行白名单（'+allowCount+' 条）';modeEl.style.color='#3fb950';}
    else{modeEl.textContent='黑名单模式：未命中黑名单默认放行';modeEl.style.color='#d29922';}
    var blk=d.dnsBlockedRecent||[], blkTotal=d.dnsBlockedTotal||0;
    document.getElementById('dnsBlkCard').textContent=blkTotal+' 次 / '+blk.length+' 域名';
    document.getElementById('dnsBlkTotal').textContent=blkTotal;
    document.getElementById('ipDrops').textContent=d.ipBlockDrops||0;
    document.getElementById('ipCache').textContent=d.ipCacheCount||0;
    document.getElementById('ipPeak').textContent=d.ipCachePeak||0;
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
  }catch(e){}
  busy=false;
}
async function saveAp(){
  var body=new URLSearchParams();body.set('ssid',document.getElementById('apSsidIn').value.trim());
  var p=document.getElementById('apPassIn').value; if(p)body.set('pass',p);
  var r=await fetch(q('/api/wifi'),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
  setMsg('apMsg',r.ok?'已保存，设备将重启':'保存失败',r.ok); if(r.status===403)showPin();
}
async function saveBridge(){
  var qs='upstreamEnable='+(document.getElementById('upEn').checked?1:0)+
    '&dnsEnable='+(document.getElementById('dnsEn').checked?1:0)+
    '&ipBlockerEnabled='+(document.getElementById('ipBlkEn').checked?1:0)+
    '&dnsAllow='+encodeURIComponent(document.getElementById('dnsAllow').value)+
    '&dnsBlock='+encodeURIComponent(document.getElementById('dnsBlock').value);
  var r=await fetch(q('/api/wifi-bridge/set?'+qs));
  setMsg('dnsMsg',r.ok?'已保存':'保存失败',r.ok);
  if(r.status===403){
    // Keep the local edits (e.g. the hermes tail entry) and retry after PIN.
    showPin(); pendingAfterPin=saveBridge; return false;
  }
  refresh();
  return r.ok;
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
function applyHermes(want){
  var ta=document.getElementById('dnsAllow');
  var hermes='hermes-prd.vn.cloud.tesla.cn';
  var parts=(ta.value||'').trim().split(/\s+/).filter(Boolean);
  parts=parts.filter(function(x){return x!==hermes;});
  if(want) parts.push(hermes);
  ta.value=parts.join(' ');
}
async function toggleHermes(){
  var box=document.getElementById('hermesControl');
  if(!box.checked){
    // Disabling is safe and immediate: just remove it from the tail.
    applyHermes(false);
    await saveBridge();
    return;
  }
  // Never enable on a single click: revert and require the 10s confirmation.
  box.checked=false;
  showHermesConfirm();
}
async function resetDns(which){
  var r=await fetch(q('/api/wifi-bridge/set?dnsReset='+which));
  setMsg('dnsMsg',r.ok?(which==='allow'?'白名单已恢复默认':'黑名单已恢复默认'):'恢复默认失败',r.ok);
  if(r.status===403)showPin();
  if(r.ok)refresh();
}
async function saveIpBlk(){
  var v=document.getElementById('ipBlkEn').checked?1:0;
  var r=await fetch(q('/api/wifi-bridge/set?ipBlockerEnabled='+v));
  setMsg('dnsMsg',r.ok?(v?'IP 层拦截已开启':'IP 层拦截已关闭'):'保存失败',r.ok);
  if(r.status===403)showPin();
  if(!r.ok)refresh();
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
document.getElementById('dnsSave').onclick=saveBridge;
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
  await saveBridge();
};
document.getElementById('dnsBlkClear').onclick=dnsBlkClear;
document.getElementById('dnsBlkDl').onclick=dnsBlkDl;
document.getElementById('fsdSave').onclick=saveFsd;
document.getElementById('fsdEn').onchange=saveFsd;
document.getElementById('fsdAutoOff').onchange=saveFsd;
document.getElementById('fsdMode').onchange=saveFsd;
document.getElementById('upEn').onchange=saveBridge;
document.getElementById('dnsEn').onchange=saveBridge;
document.getElementById('ipBlkEn').onchange=saveIpBlk;
document.getElementById('otaBtn').onclick=otaUpload;
document.getElementById('otaCheck').onclick=otaCheck;
document.getElementById('otaPull').onclick=otaPull;
document.getElementById('logClear').onclick=logClear;
document.getElementById('reboot').onclick=reboot;
document.getElementById('pinBtn').onclick=async function(){
  var r=await fetch('/api/pin',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pin='+encodeURIComponent(document.getElementById('pinInput').value)});
  if(r.ok){token=(await r.text()).trim();try{sessionStorage.setItem('fsd_tok',token);}catch(e){}document.getElementById('pinOverlay').style.display='none';
    if(pendingAfterPin){var fn=pendingAfterPin;pendingAfterPin=null;fn();}else refresh();}
  else setMsg('pinMsg','PIN 错误',false);
};
document.addEventListener('visibilitychange',function(){if(!document.hidden)refresh();});
refresh();setInterval(refresh,5000);
</script>
</body></html>)rawliteral";
