#pragma once
// PC-style FSD dashboard + field editor, served on the 10010 management port.
// Mirrors TeslaDG/app/web/fsd_panel.py: cards, handshake table, timeline,
// recent requests, API distribution, config history, current payload, and a
// field editor with add/remove/restore-default.

static constexpr char FSD_HTML[] = R"rawliteral(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FSD 拦截实况 · PC 风格</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0d1117;color:#e6edf3;font:14px/1.5 "Segoe UI",system-ui,sans-serif}
header{padding:12px 18px;border-bottom:1px solid #30363d;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
h1{font-size:16px;margin:0}
.badge{border:1px solid #30363d;border-radius:999px;padding:2px 9px;font-size:12px;color:#8b949e}
.badge.ok{color:#3fb950;border-color:#238636}.badge.warn{color:#d29922;border-color:#9e6a03}.badge.bad{color:#f85149;border-color:#da3633}
.dim{color:#8b949e}
.wrap{padding:16px 18px;max-width:1280px;margin:0 auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-bottom:14px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px 12px}
.k{color:#8b949e;font-size:12px}.v{font-size:19px;font-weight:600;margin-top:3px;word-break:break-all}
.v.s{font-size:13px;font-weight:500}
section{background:#161b22;border:1px solid #30363d;border-radius:8px;margin-bottom:12px}
h2{font-size:12px;margin:0;padding:9px 12px;border-bottom:1px solid #30363d;color:#8b949e;font-weight:600}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{text-align:left;padding:6px 9px;border-bottom:1px solid #21262d;vertical-align:top}
th{color:#8b949e;font-weight:500;white-space:nowrap}
tr:last-child td{border-bottom:0}
code{font-family:Consolas,monospace;color:#58a6ff}
pre{margin:0;padding:10px 12px;font:12px/1.5 Consolas,monospace;color:#a5d6ff;white-space:pre-wrap;word-break:break-all;max-height:420px;overflow:auto}
a{color:#58a6ff}
.btn{display:inline-block;padding:5px 11px;border:1px solid #30363d;border-radius:6px;background:#21262d;color:#e6edf3;text-decoration:none;font-size:12px;cursor:pointer}
.btn.primary{background:#1f6feb;border-color:#1f6feb}
.btn.green{background:#238636;border-color:#238636}
.btn.red{background:#da3633;border-color:#da3633}
.btn:disabled{opacity:.5;cursor:default}
.toolbar{display:flex;gap:8px;flex-wrap:wrap;padding:10px 12px;border-bottom:1px solid #30363d}
.hint{padding:8px 12px;color:#8b949e;font-size:12px}
input[type=text],textarea{width:100%;background:#0e1419;color:#d7dee3;border:1px solid #2f3c48;border-radius:6px;padding:5px 7px;font:12.5px Consolas,monospace}
textarea{height:300px;resize:vertical}
.rows td input.name{min-width:190px}.rows td input.value{min-width:110px}
.msg{padding:8px 12px;font-size:12.5px;color:#8b949e}
.msg.ok{color:#3fb950}.msg.bad{color:#f85149}
.overlay{position:fixed;inset:0;background:rgba(0,0,0,.72);display:none;align-items:center;justify-content:center;z-index:20}
.box{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:20px;width:min(360px,92vw)}
.box h3{margin:0 0 10px;font-size:16px}.box input{margin:8px 0}
.foot{padding:10px 12px;color:#8b949e;font-size:12px}
@media(max-width:700px){.wrap{padding:10px}header{padding:10px}.card .v{font-size:16px}}
</style>
<body>
<header>
  <h1>FSD 拦截实况 · PC 风格</h1>
  <span class="badge" id="hsBadge">握手：--</span>
  <span class="badge" id="linkBadge">链路：--</span>
  <span class="dim" id="nowText"></span>
  <a class="btn" href="/">控制面板</a>
  <a class="btn" href="#config">填写配置</a>
</header>
<div class="wrap">
  <div class="grid">
    <div class="card"><div class="k">拦截次数 feature_configuration</div><div class="v" id="cHits">--</div></div>
    <div class="card"><div class="k">最后拦截时间</div><div class="v s" id="cLastHit">--</div></div>
    <div class="card"><div class="k">车机 VIN</div><div class="v s"><code id="cVin">--</code></div></div>
    <div class="card"><div class="k">当前载荷（下次生效）</div><div class="v s" id="cPayload">--</div></div>
    <div class="card"><div class="k">车机状态</div><div class="v s" id="cCar">--</div></div>
    <div class="card"><div class="k">最后下发成功</div><div class="v s" id="cLastOk">--</div></div>
    <div class="card"><div class="k">车机请求总数</div><div class="v" id="cReqs">--</div></div>
    <div class="card"><div class="k">FSD DNS 重定向</div><div class="v s" id="cDns">--</div></div>
  </div>

  <section><h2>车机连接 &amp; 握手</h2>
    <table><thead><tr><th>时间</th><th>事件</th><th>客户端</th><th>SNI</th><th>第一条请求</th><th>TLS / 详情</th></tr></thead><tbody id="hsRows"></tbody></table>
  </section>

  <section><h2>下发时间线（配置变更 → 车机取配置）</h2>
    <table><thead><tr><th>时间</th><th>类型</th><th>VIN / 字段</th><th>车机上送</th><th>回包</th><th>md5</th><th>链路</th></tr></thead><tbody id="tlRows"></tbody></table>
  </section>

  <section><h2>车机最近请求</h2>
    <table><thead><tr><th>时间</th><th>方法</th><th>路径</th><th>重复</th><th>类型</th></tr></thead><tbody id="recRows"></tbody></table>
  </section>

  <section><h2>接口调用分布</h2>
    <table><thead><tr><th>接口</th><th>次数</th></tr></thead><tbody id="pathRows"></tbody></table>
  </section>

  <section><h2>配置变更记录</h2>
    <table><thead><tr><th>时间</th><th>来源</th><th>字节</th><th>md5</th><th>字段数</th></tr></thead><tbody id="cfgRows"></tbody></table>
  </section>

  <section><h2>当前回包载荷 · <code id="payloadMd5"></code></h2><pre id="payloadPre">--</pre></section>

  <section><h2>FSD 链路计数</h2>
    <table><thead><tr><th>命中</th><th>观察</th><th>透传</th><th>DNS 重定向</th><th>握手成功/失败</th><th>上游</th></tr></thead><tbody id="cntRows"></tbody></table>
  </section>

  <section id="config"><h2>下发配置（字段编辑 / 原始 JSON）</h2>
    <div class="toolbar">
      <button class="btn" id="addBtn">＋ 添加参数</button>
      <button class="btn green" id="saveBtn">保存并下发</button>
      <button class="btn" id="defaultBtn">恢复默认（22 字段）</button>
      <button class="btn" data-preset="default">默认 653B</button>
      <button class="btn" data-preset="windows">Windows 602B</button>
      <button class="btn" data-preset="16f">16 字段 476B</button>
      <button class="btn" data-preset="282b">官方 282B</button>
      <button class="btn" id="rawBtn">原始 JSON</button>
      <button class="btn" id="reloadBtn">重新读取</button>
    </div>
    <div class="hint">字段说明为中文注释，非官方定义；保存时 <code>autoshift_dr</code> 固定放在最后。默认载荷在 Windows 20 字段基础上加入 <code>enable_eu_fsd_behavior:false</code> 与 <code>autoshift_dr:true</code>。</div>
    <table class="rows"><thead><tr><th style="width:26%">字段名</th><th style="width:24%">值（true/false/字符串）</th><th>中文说明</th><th style="width:60px">操作</th></tr></thead><tbody id="rows"></tbody></table>
    <textarea id="rawJson" style="display:none"></textarea>
    <div class="msg" id="cfgMsg"></div>
    <div class="foot">保存后写入 SPIFFS，重启保持；恢复默认/预设会立即覆盖当前载荷。非 FSD 透传请求只进日志，不影响此载荷。</div>
  </section>
</div>

<div class="overlay" id="pinOverlay"><div class="box">
  <h3>输入管理 PIN</h3>
  <div class="dim" style="font-size:12px">设备设置了 PIN，需要验证后才能查看/保存。</div>
  <input type="password" id="pinInput" inputmode="numeric" autocomplete="off" placeholder="PIN">
  <button class="btn primary" id="pinBtn">进入</button>
  <div class="msg bad" id="pinMsg"></div>
</div></div>

<script>
'use strict';
var token = '';
try { token = sessionStorage.getItem('fsd_tok') || ''; } catch(e) {}
var editing = false, rawVisible = false, editorLoaded = false, busy = false;
var DEFAULT_OBJ = {"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":false,"wifi_disabled_mode":true,"enable_eu_fsd_behavior":false,"autoshift_dr":true};
var NOTES = {
  audio_log_alsa_device_delay_verbose:'音频 ALSA 设备延迟详细日志',
  audio_log_awe_stats:'音频 AWE 统计日志',
  delivered:'已交付/已激活',
  disable_eng_voice_recording_uploads:'禁止上传工程语音录音',
  drive_on_nav_maps:'导航地图上行驶',
  enable_autocomplete_telemetry:'启用自动补全遥测',
  enable_failure_log_upload:'启用故障日志上传',
  enable_fsd_auto_nav:'启用 FSD 自动导航',
  ethtrace_auto_capture:'以太网抓包自动捕获',
  ethtrace_auto_upload:'以太网抓包自动上传',
  fsd_navigation_supported:'支持 FSD 导航',
  fsd_stops_maps:'FSD 地图停车点',
  full_self_driving:'完全自动驾驶',
  latched_delivered:'锁存已交付状态',
  log_upload_without_wifi:'无 Wi-Fi 也上传日志',
  tcpdump_enabled:'启用 tcpdump',
  suspend_full_self_driving:'暂停完全自动驾驶',
  enable_seat_adjustment_from_ui:'允许 UI 调整座椅',
  disable_cabin_camera:'禁用车内摄像头（默认 false）',
  wifi_disabled_mode:'Wi-Fi 禁用模式',
  enable_eu_fsd_behavior:'启用欧盟 FSD 行为（默认 false）',
  autoshift_dr:'自动换挡（默认 true，固定最后）'
};
function q(url){ return url + (token ? (url.indexOf('?')>=0?'&':'?')+'token='+encodeURIComponent(token) : ''); }
function esc(v){ return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];}); }
function showPin(){ document.getElementById('pinOverlay').style.display='flex'; setTimeout(function(){document.getElementById('pinInput').focus();},50); }
function hidePin(){ document.getElementById('pinOverlay').style.display='none'; }
function setMsg(id,text,ok){ var el=document.getElementById(id); el.textContent=text||''; el.className='msg '+(ok===true?'ok':(ok===false?'bad':'')); }
async function getText(url){
  var r=await fetch(q(url),{cache:'no-store'});
  if(r.status===403){ showPin(); throw new Error('auth'); }
  if(!r.ok) throw new Error('HTTP '+r.status);
  return await r.text();
}
async function getJson(url){ return JSON.parse(await getText(url)); }
function parseJsonl(text){ return (text||'').trim().split('\n').filter(Boolean).map(function(l){ try{return JSON.parse(l);}catch(e){return null;} }).filter(Boolean); }
function tsText(ts){ if(!ts) return '—'; var s=String(ts).replace('T',' '); return s.slice(0,19); }
function ageText(sec){ if(sec==null) return '—'; if(sec<60) return sec+' 秒前'; if(sec<3600) return Math.floor(sec/60)+' 分钟前'; return Math.floor(sec/3600)+' 小时前'; }
function ageFrom(ts){ var t=Date.parse(ts); if(isNaN(t)) return null; return Math.max(0,Math.floor((Date.now()-t)/1000)); }
function pathOf(url){ var s=String(url||''); s=s.replace(/^https?:\/\/[^/]+/,''); s=s.replace(/\?.*$/,''); s=s.replace(/\/vehicles\/[A-Za-z0-9-]{5,}\//,'/vehicles/<VIN>/'); return s||'/'; }
function methodPath(r){ return (r.method||'?')+' '+pathOf(r.url); }
function kindBadge(kind){ var k=String(kind||'HIT'); var c=k==='HIT'?'#3fb950':(k==='OBSERVE'?'#d29922':'#8b949e'); return '<span style="color:'+c+';font-weight:600">'+esc(k)+'</span>'; }
function valText(v){ if(v===true) return 'true'; if(v===false) return 'false'; if(v===null) return 'null'; return String(v); }

async function refresh(){
  if(document.hidden || busy) return; busy=true;
  try{
    // Sequential requests: the ESP32 AsyncWebServer is much more reliable
    // with one 4 KB response at a time than five parallel fetches.
    var st=await getJson('/api/wifi-bridge/status');
    var hits=parseJsonl(await getText('/api/fsd-mock/hits'));
    var hs=parseJsonl(await getText('/api/fsd-mock/handshakes'));
    var cfg=parseJsonl(await getText('/api/fsd-mock/config-history'));
    var payload=await getText('/api/fsd-mock/payload');
    render(st,hits,hs,cfg,payload);
  }catch(e){ /* auth overlay already shown or transient */ }
  busy=false;
}
function render(st,hits,hs,cfg,payloadText){
  var fm=st.fsdMock||{}; var now=new Date();
  document.getElementById('nowText').textContent=now.toLocaleString();
  var hsOk=fm.handshake?fm.handshake.ok||0:0, hsFail=fm.handshake?fm.handshake.fail||0:0;
  var hsBadge=document.getElementById('hsBadge');
  hsBadge.textContent=hsOk>0?('握手：TLS 已建立 ('+(fm.handshake.tls||'TLS')+')'):('握手：'+(hsFail>0?'失败':'未建立'));
  hsBadge.className='badge '+(hsOk>0?'ok':(hsFail>0?'bad':''));
  var linkBadge=document.getElementById('linkBadge');
  linkBadge.textContent=st.upstreamConnected?('链路：'+esc(st.upstreamSSID||'已连接')):'链路：未连接';
  linkBadge.className='badge '+(st.upstreamConnected?'ok':'bad');

  var intercepts=hits.filter(function(r){return r.kind==='HIT'||r.kind==='OBSERVE';});
  var lastHit=intercepts[0]||null;
  var vin='';
  for(var i=0;i<hits.length;i++){ if(hits[i].vin){ vin=hits[i].vin; break; } }
  var totalReqs=(fm.hits||0)+(fm.observes||0)+(fm.flows||0);
  document.getElementById('cHits').textContent=(fm.hits||0);
  document.getElementById('cLastHit').textContent=lastHit?tsText(lastHit.ts):'—';
  document.getElementById('cVin').textContent=vin||'—';
  document.getElementById('cPayload').textContent=(fm.bytes||0)+' B / '+(fm.md5||'').slice(0,10)+(fm.fields?(' / '+fm.fields+' 字段'):'');
  var recentFsd=(fm.recent||[])[0];
  var lastAge=recentFsd?Math.max(0,Math.floor(((fm.nowMs||0)-recentFsd.atMs)/1000)):null;
  var carEl=document.getElementById('cCar');
  if(lastAge!=null && lastAge<180){ carEl.textContent='在线 · '+ageText(lastAge); carEl.style.color='#3fb950'; }
  else if(totalReqs>0){ carEl.textContent='链路可达 · 最后 '+(lastHit?tsText(lastHit.ts):'—'); carEl.style.color='#d29922'; }
  else { carEl.textContent='离线 · 等待车机请求'; carEl.style.color='#f85149'; }
  document.getElementById('cLastOk').textContent=lastHit?ageText(ageFrom(lastHit.ts)):'—';
  document.getElementById('cReqs').textContent=totalReqs;
  document.getElementById('cDns').textContent=(fm.dnsRedirects||0)+(fm.dnsLast?(' / '+fm.dnsLast):'');

  var hsRows='';
  hs.slice(0,8).forEach(function(h){
    var ev=h.event||''; var badge='';
    if(ev==='handshake_ok') badge='<span style="color:#3fb950">握手成功</span>';
    else if(ev==='handshake_failed') badge='<span style="color:#f85149">握手失败</span>';
    else badge='<span style="color:#d29922">ClientHello</span>';
    var detail=ev==='handshake_ok'?((h.tls||'-')+' / '+(h.cipher||'-')):(h.error||h.sni||'-');
    hsRows+='<tr><td>'+esc(tsText(h.ts))+'</td><td>'+badge+'</td><td>'+esc(h.client||'-')+'</td><td><code>'+esc(h.sni||'-')+'</code></td><td><code>'+esc(h.first_request||'-')+'</code></td><td>'+esc(detail)+'</td></tr>';
  });
  document.getElementById('hsRows').innerHTML=hsRows||'<tr><td colspan="6" class="dim">还没有握手记录（车机尚未发起连接）</td></tr>';

  var timeline=[];
  intercepts.forEach(function(r){ timeline.push({ts:r.ts||'',kind:'hit',r:r}); });
  var lastMd5=null;
  cfg.forEach(function(r){ if(!r.bytes) return; if(r.md5 && r.md5===lastMd5) return; lastMd5=r.md5; timeline.push({ts:r.ts||'',kind:'cfg',r:r}); });
  timeline.sort(function(a,b){ return String(b.ts).localeCompare(String(a.ts)); });
  var tl='';
  timeline.slice(0,20).forEach(function(x){
    var r=x.r;
    if(x.kind==='hit'){
      tl+='<tr><td>'+esc(tsText(r.ts))+'</td><td><span style="color:#3fb950">车机取配置</span> '+esc(r.method||'')+'</td><td><code>'+esc(r.vin||'—')+'</code></td><td><code>'+esc(r.req_head||'—')+'</code><br>'+esc(r.req_bytes||0)+' B</td><td style="color:#3fb950">'+esc(r.bytes||0)+' B / '+esc(r.mode||'')+'</td><td><code>'+esc((r.md5||'').slice(0,14))+'…</code></td><td>'+esc(r.http_version||'')+'</td></tr>';
    } else {
      tl+='<tr><td>'+esc(tsText(r.ts))+'</td><td><span style="color:#8b949e">配置变更</span> '+esc(r.source||'')+'</td><td>'+esc(r.fields||0)+' 字段</td><td>—</td><td>'+esc(r.bytes||0)+' B</td><td><code>'+esc((r.md5||'').slice(0,14))+'…</code></td><td>本地保存</td></tr>';
    }
  });
  document.getElementById('tlRows').innerHTML=tl||'<tr><td colspan="7" class="dim">还没有记录</td></tr>';

  var collapsed=[];
  hits.forEach(function(r){
    var key=methodPath(r);
    if(collapsed.length && collapsed[collapsed.length-1].key===key){ collapsed[collapsed.length-1].n++; collapsed[collapsed.length-1].ts=r.ts; collapsed[collapsed.length-1].kind=r.kind; }
    else collapsed.push({key:key,method:r.method||'',path:pathOf(r.url),n:1,ts:r.ts,kind:r.kind||''});
  });
  var rec='';
  collapsed.slice(0,18).forEach(function(c){
    rec+='<tr><td>'+esc(tsText(c.ts))+'</td><td>'+esc(c.method)+'</td><td><code>'+esc(c.path)+'</code></td><td>'+(c.n>1?('×'+c.n):'')+'</td><td>'+kindBadge(c.kind)+'</td></tr>';
  });
  document.getElementById('recRows').innerHTML=rec||'<tr><td colspan="5" class="dim">还没有车机请求</td></tr>';

  var counts={};
  hits.forEach(function(r){ var k=methodPath(r); counts[k]=(counts[k]||0)+1; });
  var paths=Object.keys(counts).sort(function(a,b){return counts[b]-counts[a];}).slice(0,12);
  document.getElementById('pathRows').innerHTML=paths.map(function(k){return '<tr><td><code>'+esc(k)+'</code></td><td>'+counts[k]+'</td></tr>';}).join('')||'<tr><td colspan="2" class="dim">-</td></tr>';

  var cfgHtml='';
  cfg.slice(0,12).forEach(function(r){ cfgHtml+='<tr><td>'+esc(tsText(r.ts))+'</td><td>'+esc(r.source||'')+'</td><td>'+esc(r.bytes||0)+'</td><td><code>'+esc((r.md5||'').slice(0,14))+'…</code></td><td>'+esc(r.fields||0)+'</td></tr>'; });
  document.getElementById('cfgRows').innerHTML=cfgHtml||'<tr><td colspan="5" class="dim">还没有配置变更记录</td></tr>';

  var pretty=payloadText||''; try{ pretty=JSON.stringify(JSON.parse(payloadText),null,2); }catch(e){}
  document.getElementById('payloadPre').textContent=pretty||'--';
  document.getElementById('payloadMd5').textContent=fm.md5||'';
  document.getElementById('cntRows').innerHTML='<tr><td>'+(fm.hits||0)+'</td><td>'+(fm.observes||0)+'</td><td>'+(fm.flows||0)+'</td><td>'+(fm.dnsRedirects||0)+'</td><td>'+hsOk+' / '+hsFail+'</td><td>'+esc(st.upstreamConnected?(st.upstreamSSID+' '+st.upstreamIP):'未连接')+'</td></tr>';

  if(!editorLoaded){ editorLoaded=true; try{ renderRows(JSON.parse(payloadText)); }catch(e){} }
}
function noteFor(name){ return NOTES[name] || '自定义字段'; }
function makeRow(name,value){
  var tr=document.createElement('tr');
  tr.innerHTML='<td><input class="name" type="text" value="'+esc(name)+'"></td>'+
    '<td><input class="value" type="text" value="'+esc(valText(value))+'"></td>'+
    '<td class="note">'+esc(noteFor(name))+'</td>'+
    '<td><button class="btn red del" type="button">删除</button></td>';
  tr.querySelector('.del').onclick=function(){ tr.remove(); editing=true; };
  tr.querySelectorAll('input').forEach(function(inp){ inp.oninput=function(){ editing=true; var n=tr.querySelector('.name').value.trim(); tr.querySelector('.note').textContent=noteFor(n); }; });
  return tr;
}
function autoRow(){ var rows=document.getElementById('rows').querySelectorAll('tr'); for(var i=0;i<rows.length;i++){ if(rows[i].querySelector('.name').value.trim()==='autoshift_dr') return rows[i]; } return null; }
function insertRow(tr){ var tbody=document.getElementById('rows'); var a=autoRow(); if(a) tbody.insertBefore(tr,a); else tbody.appendChild(tr); }
function renderRows(obj){
  var tbody=document.getElementById('rows'); tbody.innerHTML='';
  var entries=Object.keys(obj||{}).map(function(k){return [k,obj[k]];});
  var hasEu=false, auto=null;
  entries.forEach(function(e){ if(e[0]==='autoshift_dr'){ auto=e; return; } if(e[0]==='enable_eu_fsd_behavior') hasEu=true; tbody.appendChild(makeRow(e[0],e[1])); });
  if(!hasEu) tbody.appendChild(makeRow('enable_eu_fsd_behavior',false));
  tbody.appendChild(makeRow('autoshift_dr',auto?auto[1]:true));
  editing=false;
}
function coerce(v){ var s=String(v).trim(); if(s==='true') return true; if(s==='false') return false; if(s==='null') return null; return s; }
function readRows(){
  var obj={}, auto;
  var rows=document.getElementById('rows').querySelectorAll('tr');
  for(var i=0;i<rows.length;i++){
    var n=rows[i].querySelector('.name').value.trim(); if(!n) continue;
    var v=coerce(rows[i].querySelector('.value').value);
    if(n==='autoshift_dr'){ auto=v; continue; }
    obj[n]=v;
  }
  if(auto!==undefined) obj.autoshift_dr=auto;
  return obj;
}
function addRow(){ insertRow(makeRow('','')); editing=true; }
async function savePayload(){
  var obj;
  try{ obj=rawVisible?JSON.parse(document.getElementById('rawJson').value):readRows(); }
  catch(e){ setMsg('cfgMsg','JSON 格式错误：'+e.message,false); return; }
  if(!obj || typeof obj!=='object' || Array.isArray(obj)){ setMsg('cfgMsg','顶层必须是 JSON 对象',false); return; }
  if(Object.prototype.hasOwnProperty.call(obj,'autoshift_dr')){ var av=obj.autoshift_dr; delete obj.autoshift_dr; obj.autoshift_dr=av; }
  var r=await fetch(q('/api/fsd-mock/payload'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  var text=await r.text();
  if(r.status===200){ setMsg('cfgMsg','已保存并下发：'+JSON.stringify(obj).length+' B',true); editing=false; editorLoaded=false; refresh(); }
  else { var why='保存失败'; try{ why=JSON.parse(text).error||why; }catch(e){} setMsg('cfgMsg',why,false); if(r.status===403) showPin(); }
}
async function applyPreset(name){
  if(!confirm('将立即覆盖当前载荷并保存，继续？')) return;
  var r=await fetch(q('/api/fsd-mock/set?preset='+encodeURIComponent(name)));
  var text=await r.text();
  if(r.status===200){ setMsg('cfgMsg','已加载预设 '+name,true); editing=false; editorLoaded=false; refresh(); }
  else { var why='加载失败'; try{ why=JSON.parse(text).error||why; }catch(e){} setMsg('cfgMsg',why,false); if(r.status===403) showPin(); }
}
document.getElementById('addBtn').onclick=addRow;
document.getElementById('saveBtn').onclick=savePayload;
document.getElementById('reloadBtn').onclick=function(){ editing=false; editorLoaded=false; refresh(); };
document.getElementById('defaultBtn').onclick=function(){ renderRows(DEFAULT_OBJ); setMsg('cfgMsg','已填入默认 22 字段（含 autoshift_dr），点击“保存并下发”生效',true); };
document.getElementById('rawBtn').onclick=function(){
  var ta=document.getElementById('rawJson'), table=document.querySelector('.rows');
  if(!rawVisible){ ta.value=JSON.stringify(readRows(),null,2); ta.style.display='block'; table.style.display='none'; rawVisible=true; this.textContent='表格编辑'; }
  else { try{ renderRows(JSON.parse(ta.value)); ta.style.display='none'; table.style.display=''; rawVisible=false; this.textContent='原始 JSON'; }catch(e){ setMsg('cfgMsg','JSON 格式错误：'+e.message,false); } }
};
document.querySelectorAll('[data-preset]').forEach(function(b){ b.onclick=function(){ applyPreset(b.getAttribute('data-preset')); }; });
document.getElementById('pinBtn').onclick=async function(){
  var pin=document.getElementById('pinInput').value;
  var r=await fetch('/api/pin',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pin='+encodeURIComponent(pin)});
  if(r.ok){ token=(await r.text()).trim(); try{sessionStorage.setItem('fsd_tok',token);}catch(e){} hidePin(); setMsg('pinMsg','',true); refresh(); }
  else { setMsg('pinMsg','PIN 错误',false); }
};
document.getElementById('pinInput').addEventListener('keydown',function(e){ if(e.key==='Enter') document.getElementById('pinBtn').click(); });
fetch('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ts='+Math.floor(Date.now()/1000)}).catch(function(){});
document.addEventListener('visibilitychange',function(){ if(!document.hidden) refresh(); });
refresh();
setInterval(refresh,5000);
</script>
</body></html>)rawliteral";
