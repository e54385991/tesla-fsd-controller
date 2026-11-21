#pragma once

static constexpr char CLIENT_CONFIG_HTML[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>客户端配置</title>
<style>
  * { box-sizing: border-box }
  body { margin: 36px auto; max-width: 660px; padding: 0 16px; background: #0f172a;
         color: #e2e8f0; font: 16px system-ui, sans-serif }
  label, textarea { display: block; margin-top: 16px }
  input, textarea, button { font: inherit; padding: 8px 12px; border-radius: 6px;
                             color: inherit; background: #1e293b; border: 1px solid #64748b }
  input, textarea { width: 100% }
  textarea { min-height: 180px; resize: vertical; font-family: monospace }
  button { margin: 12px 8px 0 0; cursor: pointer }
  button:disabled { opacity: .5; cursor: default }
  #status { min-height: 1.3em }
</style>
<main>
  <h1>客户端配置</h1>
  <label for="token">管理令牌</label>
  <input id="token" type="password" autocomplete="off" maxlength="64" spellcheck="false">
  <button id="load">读取</button>
  <p id="version"></p>
  <label for="config">JSON 配置</label>
  <textarea id="config" spellcheck="false" disabled></textarea>
  <button id="save" disabled>发布</button>
  <button id="rollback" disabled>回滚到上一版</button>
  <p id="status" role="status"></p>
</main>
<script>
  const token = document.getElementById('token');
  const editor = document.getElementById('config');
  const version = document.getElementById('version');
  const status = document.getElementById('status');
  const load = document.getElementById('load');
  const save = document.getElementById('save');
  const rollback = document.getElementById('rollback');
  let state = null;
  function lock(busy) {
    load.disabled = token.disabled = busy;
    editor.disabled = save.disabled = busy || !state;
    rollback.disabled = busy || !state?.canRollback;
  }
  async function update(method, suffix, body, message) {
    if (!/^[0-9a-f]{64}$/.test(token.value)) {
      status.textContent = '请输入完整的管理令牌';
      return;
    }
    lock(true);
    status.textContent = '';
    try {
      const response = await fetch('/api/client-config' + suffix, {
        method,
        headers: {Authorization: 'Bearer ' + token.value, 'Content-Type': 'application/json'},
        body: body === undefined ? undefined : JSON.stringify(body)
      });
      if (response.status === 409) {
        state = null;
        throw Error('版本已变更，请重新读取');
      }
      if (response.status === 401) throw Error('令牌无效');
      if (response.status === 413) throw Error('JSON 配置最多 1024 字节');
      if (!response.ok) throw Error('设备返回 ' + response.status);
      state = await response.json();
      editor.value = JSON.stringify(state.config, null, 2);
      version.textContent = `当前版本 ${state.revision} · 客户端确认版本 ${state.applied || '尚未确认'}`;
      status.textContent = message;
    } catch (error) {
      status.textContent = error.message;
    } finally {
      lock(false);
    }
  }
  token.oninput = () => { state = null; version.textContent = editor.value = ''; lock(false); };
  load.onclick = () => update('GET', '', undefined, '已读取');
  save.onclick = () => {
    try {
      const config = JSON.parse(editor.value);
      if (!config || typeof config !== 'object' || Array.isArray(config)) throw Error();
      update('PUT', '', {revision: state.revision, config}, '已保存，等待客户端确认');
    } catch {
      status.textContent = '请输入有效的 JSON 对象';
    }
  };
  rollback.onclick = () => update('POST', '/rollback', {revision: state.revision}, '已回滚，等待客户端确认');
</script>
</html>)HTML";
