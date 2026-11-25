#pragma once
// Web page served at "/". Talks to /status, /list, /download, /delete, /upload, /log, /reboot.

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KLDongle</title>
<style>
  :root {
    --accent: #e3681b; --accent-dk: #c4550f; --ink: #1d1d1f; --muted: #6e6e73; --line: #e6e6e9;
    --soft: #f5f5f7; --panel: #fff; --bg: #111; --ok: #2e9d4f; --warn: #e3681b; --bad: #d33; --red: #c62828;
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 24px 16px; background: var(--bg); color: var(--ink);
         font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; }
  .wrap { max-width: 880px; margin: 0 auto; background: var(--panel); border-radius: 10px; overflow: hidden; }

  header { display: flex; flex-wrap: wrap; gap: 4px 16px; align-items: baseline; justify-content: space-between;
           padding: 14px 20px; border-bottom: 3px solid var(--accent); }
  header h1 { margin: 0; font-size: 18px; font-weight: 650; }
  header .net { color: var(--muted); font-size: 13px; font-variant-numeric: tabular-nums; }

  .status { display: grid; grid-template-columns: repeat(4, 1fr); border-bottom: 1px solid var(--line); }
  .stat { padding: 10px 20px; border-right: 1px solid var(--line); min-width: 0; }
  .stat:last-child { border-right: 0; }
  .stat .k { font-size: 11px; color: var(--muted); text-transform: uppercase; letter-spacing: .05em; }
  .stat .v { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .stat .s { font-size: 12px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; vertical-align: 1px; background: #c7c7cc; }
  .dot.on { background: var(--ok); } .dot.warn { background: var(--warn); } .dot.bad { background: var(--bad); }
  .dot.roast { animation: breathe 4s ease-in-out infinite; }
  @keyframes breathe { 0%, 100% { background: #e01b1b; } 50% { background: #f08a1c; } }

  .toolbar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; padding: 12px 20px; }
  .crumbs { flex: 1 1 auto; min-width: 0; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .crumbs a { color: var(--accent); text-decoration: none; }
  .crumbs a:hover { text-decoration: underline; }
  .crumbs .sep { color: var(--muted); margin: 0 4px; font-weight: 400; }
  .btn { font: inherit; font-size: 13px; font-weight: 600; border: 1px solid transparent; border-radius: 6px;
         padding: 5px 12px; cursor: pointer; background: var(--accent); color: #fff; }
  .btn:hover { background: var(--accent-dk); }
  .btn.ghost { background: var(--panel); color: var(--ink); border-color: var(--line); }
  .btn.ghost:hover { background: var(--soft); }
  .btn:disabled { opacity: .5; cursor: default; }

  #msg { display: none; margin: 0 20px 10px; padding: 8px 12px; border-radius: 6px; font-size: 13px; }
  #msg.err { display: block; background: #fdecea; color: var(--red); }
  #msg.info { display: block; background: var(--soft); color: var(--muted); }

  table { width: 100%; border-collapse: collapse; }
  th { text-align: left; font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase;
       letter-spacing: .05em; padding: 6px 8px; border-bottom: 1px solid var(--line); }
  td { padding: 7px 8px; border-bottom: 1px solid var(--line); vertical-align: middle; }
  th:first-child, td:first-child { padding-left: 20px; }
  th:last-child, td:last-child { padding-right: 20px; }
  tbody tr:hover { background: #fafafa; }
  td.name { width: 100%; word-break: break-word; }
  td.name .ico { display: inline-block; width: 1.4em; color: var(--muted); }
  td.name a { color: var(--ink); text-decoration: none; font-weight: 600; cursor: pointer; }
  td.name a:hover { color: var(--accent); }
  td.num, th.num { text-align: right; white-space: nowrap; color: var(--muted); font-variant-numeric: tabular-nums; }
  td.when { white-space: nowrap; color: var(--muted); font-variant-numeric: tabular-nums; }
  td.act { white-space: nowrap; text-align: right; }
  .link { font: inherit; font-size: 13px; background: none; border: 0; padding: 2px 6px; cursor: pointer; color: var(--accent); }
  .link:hover { text-decoration: underline; }
  .link.del { color: var(--muted); }
  .link.del:hover { color: var(--red); }
  .empty { padding: 18px 20px; color: var(--muted); }

  footer { display: flex; flex-wrap: wrap; gap: 4px 16px; justify-content: space-between; padding: 12px 20px;
           color: var(--muted); font-size: 12px; background: var(--soft); }
  footer a { color: var(--muted); }
  footer a:hover { color: var(--accent); }

  @media (max-width: 640px) {
    body { padding: 0; }
    .wrap { border-radius: 0; }
    .status { grid-template-columns: 1fr 1fr; }
    .stat:nth-child(2) { border-right: 0; }
    .stat:nth-child(-n+2) { border-bottom: 1px solid var(--line); }
    .col-when { display: none; }
  }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Kaffelogic Dongle</h1>
    <span class="net" id="net">&nbsp;</span>
  </header>

  <div class="status">
    <div class="stat"><div class="k">Roaster</div><div class="v" id="sRoaster">&ndash;</div><div class="s" id="sRoaster2">&nbsp;</div></div>
    <div class="stat"><div class="k">Stick</div><div class="v" id="sStick">&ndash;</div><div class="s" id="sStick2">&nbsp;</div></div>
    <div class="stat"><div class="k">Studio</div><div class="v" id="sStudio">&ndash;</div><div class="s" id="sStudio2">&nbsp;</div></div>
    <div class="stat"><div class="k">Storage</div><div class="v" id="sCard">&ndash;</div><div class="s" id="sCard2">&nbsp;</div></div>
  </div>

  <div class="toolbar">
    <div class="crumbs" id="crumbs"></div>
    <input type="file" id="fileInput" hidden>
    <button class="btn" id="uploadBtn" title="Writing briefly unplugs the stick from the roaster. Not possible while roasting.">Upload</button>
    <button class="btn ghost" id="refreshBtn">Refresh</button>
  </div>
  <div id="msg"></div>

  <table>
    <thead><tr><th>Name</th><th class="num">Size</th><th class="col-when">Date</th><th></th></tr></thead>
    <tbody id="files"></tbody>
  </table>
  <div class="empty" id="empty" hidden>No files</div>

  <footer>
    <span>Uploading or deleting briefly unplugs the stick from the roaster; not possible while roasting.</span>
    <span><span id="ver"></span> &middot; <a href="/log" target="_blank">Log</a> &middot; <a href="#" id="rebootLink">Reboot</a></span>
  </footer>
</div>

<script>
let currentPath = '/';
const $ = id => document.getElementById(id);

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}
function formatBytes(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  if (b < 1073741824) return (b / 1048576).toFixed(1) + ' MB';
  return (b / 1073741824).toFixed(1) + ' GB';
}
function fmtDate(t) {
  return new Date(t * 1000).toLocaleString(undefined, { year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
}
function msg(text, kind) { const m = $('msg'); m.textContent = text || ''; m.className = text ? kind || 'err' : ''; }
function setStat(id, text, sub, dot) {
  const v = $(id);
  v.replaceChildren();
  if (dot !== undefined) v.appendChild(el('span', 'dot ' + dot));
  v.appendChild(document.createTextNode(text));
  v.title = text;
  $(id + '2').textContent = sub || ' ';
}

let lastStatus = {};
function refreshStatus() {
  fetch('/status').then(r => r.json()).then(s => {
    lastStatus = s;
    $('ver').textContent = 'KLDongle ' + s.version;
    $('net').textContent = s.ip + ' · ' + s.hostname + ' · ' + s.rssi + ' dBm';
    if (s.live) setStat('sRoaster', 'Roasting', s.live, 'roast');
    else setStat('sRoaster', 'Idle', s.roasterActive ? 'USB host is writing' : '', 'on');
    if (s.writeWindow) setStat('sStick', 'Away (writing)', 'back in a moment', 'warn');
    else setStat('sStick', s.usbAttached ? 'With the roaster' : 'Detached', s.allowWrite ? 'Writes allowed' : 'Read-only (ALLOW_WRITE=0)', s.usbAttached ? 'on' : 'bad');
    setStat('sStudio', s.studio ? 'Connected' : 'Not connected', s.studio ? s.studioPeer : '', s.studio ? 'on' : '');
    if (s.size) setStat('sCard', formatBytes(s.size - s.used) + ' free', s.storage + ' \u00b7 ' + formatBytes(s.used) + ' of ' + formatBytes(s.size) + ' used');
  }).catch(() => {});
}

function updateCrumbs() {
  const c = $('crumbs');
  c.replaceChildren();
  const add = (label, path, last) => {
    if (last) { c.appendChild(el('span', '', label)); return; }
    const a = el('a', '', label); a.href = '#'; a.onclick = () => { navigateTo(path); return false; };
    c.appendChild(a);
  };
  const parts = currentPath.split('/').filter(x => x);
  add('Stick', '/', parts.length === 0);
  let p = '';
  parts.forEach((part, i) => {
    p += '/' + part;
    c.appendChild(el('span', 'sep', '/'));
    add(part, p, i === parts.length - 1);
  });
}

function navigateTo(path) { currentPath = path || '/'; msg(''); updateCrumbs(); refreshFiles(); }

function refreshFiles() {
  fetch('/list?path=' + encodeURIComponent(currentPath)).then(r => r.json()).then(data => {
    const body = $('files');
    body.replaceChildren();
    if (currentPath !== '/') {
      const tr = el('tr'), td = el('td', 'name');
      const a = el('a', '', '..'); a.onclick = () => { const p = currentPath.split('/').filter(x => x); p.pop(); navigateTo('/' + p.join('/')); };
      td.append(el('span', 'ico', '↰'), a);
      tr.append(td, el('td', 'num'), el('td', 'when col-when'), el('td'));
      body.appendChild(tr);
    }
    data.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    data.forEach(f => {
      const tr = el('tr');
      const name = el('td', 'name');
      if (f.dir) {
        const a = el('a', '', f.name); a.onclick = () => navigateTo(f.path);
        name.append(el('span', 'ico', '\u{1F4C1}'), a);
      } else {
        name.append(el('span', 'ico', '\u{1F4C4}'), document.createTextNode(f.name));
      }
      let when = '';
      if (f.roasted) when = 'Roasted ' + fmtDate(f.roasted);
      else if (!f.dir && f.mtime > 1000000000 && !f.name.toLowerCase().endsWith('.klog')) when = fmtDate(f.mtime);
      const act = el('td', 'act');
      if (!f.dir) {
        const d = el('button', 'link', 'Download');
        d.onclick = () => { window.location.href = '/download?file=' + encodeURIComponent(f.path); };
        const x = el('button', 'link del', 'Delete');
        x.onclick = () => deleteFile(f.path);
        act.append(d, x);
      }
      tr.append(name, el('td', 'num', f.dir ? '' : formatBytes(f.size)), el('td', 'when col-when', when), act);
      body.appendChild(tr);
    });
    $('empty').hidden = data.length > 0;
  }).catch(() => msg('Could not read this folder'));
}

function deleteFile(path) {
  const note = lastStatus.storage === 'internal flash' ? 'This cannot be undone.' : 'Files are moved to /kl-trash; deleting inside /kl-trash is final.';
  if (!confirm('Delete ' + path + '?\n\n' + note)) return;
  fetch('/delete?file=' + encodeURIComponent(path), { method: 'DELETE' })
    .then(r => r.text().then(t => { msg(r.ok ? '' : t); refreshFiles(); refreshStatus(); }));
}

$('uploadBtn').onclick = () => $('fileInput').click();
$('fileInput').onchange = () => {
  const f = $('fileInput').files[0];
  if (!f) return;
  const fd = new FormData();
  fd.append('file', f, f.name);
  $('uploadBtn').disabled = true;
  msg('Uploading ' + f.name + '…', 'info');
  fetch('/upload?path=' + encodeURIComponent(currentPath), { method: 'POST', body: fd })
    .then(r => r.text().then(t => { msg(r.ok ? '' : t); }))
    .catch(() => msg('Upload failed'))
    .finally(() => { $('uploadBtn').disabled = false; $('fileInput').value = ''; refreshFiles(); refreshStatus(); });
};
$('refreshBtn').onclick = () => { refreshFiles(); refreshStatus(); };
$('rebootLink').onclick = () => {
  if (confirm('Reboot the dongle? The roaster loses the stick for a few seconds.')) fetch('/reboot', { method: 'POST' });
  return false;
};

updateCrumbs();
refreshFiles();
refreshStatus();
setInterval(refreshStatus, 3000);
setInterval(refreshFiles, 30000);
</script>
</body>
</html>
)HTML";
