#pragma once
// Web page served at "/". Talks to /status, /list, /download, /delete, /upload, /log, /reboot.

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KLDongle</title>
<style>
  :root { --accent: #e3681b; --ink: #222; --muted: #666; --line: #ddd; --panel: #fff; --soft: #f5f5f5; }
  * { box-sizing: border-box; }
  body { font-family: -apple-system, Segoe UI, Arial, sans-serif; margin: 0; padding: 16px; background: #000; color: var(--ink); }
  .container { max-width: 860px; margin: 0 auto; background: var(--panel); padding: 20px; border-radius: 8px; }
  h1 { margin: 0 0 12px; font-size: 1.4em; border-bottom: 2px solid var(--accent); padding-bottom: 10px; }
  h3 { margin: 18px 0 8px; }
  .status { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 8px; }
  .tile { background: var(--soft); border-radius: 6px; padding: 8px 10px; }
  .tile .k { font-size: .75em; color: var(--muted); text-transform: uppercase; letter-spacing: .04em; }
  .tile .v { font-weight: 600; margin-top: 2px; word-break: break-word; }
  .dot { display: inline-block; width: .6em; height: .6em; border-radius: 50%; margin-right: .4em; background: #bbb; }
  .on { background: #2e9d4f; } .warn { background: var(--accent); } .bad { background: #d33; }
  .breadcrumb { padding: 8px 10px; background: var(--soft); border-radius: 6px; margin: 10px 0; }
  .breadcrumb a { color: var(--accent); text-decoration: none; margin: 0 3px; }
  .file-item { padding: 8px 4px; border-bottom: 1px solid var(--line); display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
  .file-name { font-weight: 600; flex: 1 1 220px; word-break: break-all; }
  .file-name.dir { color: #2176c7; cursor: pointer; }
  .meta { color: var(--muted); font-size: .85em; flex: 0 1 auto; }
  .btn { background: var(--accent); color: #fff; border: 0; padding: 7px 14px; border-radius: 4px; cursor: pointer; font-size: .9em; }
  .btn:hover { background: #000; }
  .btn-danger { background: #c62828; }
  .upload-form { margin: 14px 0; padding: 12px; background: #f9f9f9; border-radius: 6px; }
  #msg { margin: 8px 0; min-height: 1.2em; color: #c62828; }
  a.plain { color: var(--accent); }
</style>
</head>
<body>
<div class="container">
  <h1>Kaffelogic Dongle</h1>
  <div class="status" id="status"></div>

  <div class="breadcrumb"><strong>Path:</strong> <span id="breadcrumb"></span></div>

  <div class="upload-form">
    <strong>Upload to this folder</strong><br>
    <input type="file" id="fileInput">
    <button class="btn" id="uploadBtn">Upload</button>
    <div class="meta">Writing briefly unplugs the stick from the roaster. Not possible while roasting.</div>
  </div>
  <div id="msg"></div>

  <h3>Files <button class="btn" onclick="refreshFiles()">Refresh</button></h3>
  <div id="files">Loading...</div>

  <p class="meta"><a class="plain" href="/log" target="_blank">Log</a> &middot;
     <a class="plain" href="#" onclick="reboot(); return false;">Reboot dongle</a></p>
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
  return (b / 1073741824).toFixed(2) + ' GB';
}
function fmtDate(t) { return t ? new Date(t * 1000).toLocaleString() : ''; }

function tile(k, v, dot) {
  const t = el('div', 'tile');
  t.appendChild(el('div', 'k', k));
  const vv = el('div', 'v');
  if (dot !== undefined) vv.appendChild(el('span', 'dot ' + dot));
  vv.appendChild(document.createTextNode(v));
  t.appendChild(vv);
  return t;
}

function refreshStatus() {
  fetch('/status').then(r => r.json()).then(s => {
    const box = $('status');
    box.replaceChildren(
      tile('Roaster', s.live ? 'Roasting: ' + s.live : (s.roasterActive ? 'Writing to stick' : 'Idle'), s.roasterActive ? 'warn' : 'on'),
      tile('Stick', s.writeWindow ? 'Away (writing)' : (s.usbAttached ? 'With the roaster' : 'Detached'), s.writeWindow ? 'warn' : (s.usbAttached ? 'on' : 'bad')),
      tile('Studio', s.studio ? 'Connected (' + s.studioPeer + ')' : 'Not connected', s.studio ? 'on' : ''),
      tile('Network', s.ip + ' / ' + s.hostname + ' (' + s.rssi + ' dBm)'),
      tile('Card', s.size ? formatBytes(s.used) + ' used of ' + formatBytes(s.size) : '-'),
      tile('Writes', s.allowWrite ? 'Allowed' : 'Off (ALLOW_WRITE=0)')
    );
  }).catch(() => {});
}

function updateBreadcrumb() {
  const bc = $('breadcrumb');
  bc.replaceChildren();
  const root = el('a', '', 'Root'); root.href = '#'; root.onclick = () => { navigateTo('/'); return false; };
  bc.appendChild(root);
  let p = '';
  currentPath.split('/').filter(x => x).forEach(part => {
    p += '/' + part;
    const target = p;
    bc.appendChild(document.createTextNode(' / '));
    const a = el('a', '', part); a.href = '#'; a.onclick = () => { navigateTo(target); return false; };
    bc.appendChild(a);
  });
}

function navigateTo(path) { currentPath = path || '/'; updateBreadcrumb(); refreshFiles(); }
function navigateUp() { const parts = currentPath.split('/').filter(x => x); parts.pop(); navigateTo('/' + parts.join('/')); }

function refreshFiles() {
  fetch('/list?path=' + encodeURIComponent(currentPath)).then(r => r.json()).then(data => {
    const box = $('files');
    box.replaceChildren();
    if (currentPath !== '/') {
      const row = el('div', 'file-item');
      const n = el('span', 'file-name dir', '.. (parent folder)'); n.onclick = navigateUp;
      row.appendChild(n); box.appendChild(row);
    }
    data.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    data.forEach(f => {
      const row = el('div', 'file-item');
      const n = el('span', 'file-name ' + (f.dir ? 'dir' : 'file'), (f.dir ? '\u{1F4C1} ' : '\u{1F4C4} ') + f.name);
      if (f.dir) n.onclick = () => navigateTo(f.path);
      row.appendChild(n);
      if (!f.dir) {
        let meta = formatBytes(f.size);
        if (f.roasted) meta += ' · roasted ' + fmtDate(f.roasted);
        else if (f.mtime > 1000000000 && !f.name.toLowerCase().endsWith('.klog')) meta += ' · ' + fmtDate(f.mtime);
        row.appendChild(el('span', 'meta', meta));
        const d = el('button', 'btn', 'Download'); d.onclick = () => { window.location.href = '/download?file=' + encodeURIComponent(f.path); };
        const x = el('button', 'btn btn-danger', 'Delete'); x.onclick = () => deleteFile(f.path);
        row.appendChild(d); row.appendChild(x);
      }
      box.appendChild(row);
    });
    if (!box.children.length) box.appendChild(el('p', '', 'No files'));
  }).catch(() => { $('files').textContent = 'Could not read the folder'; });
}

function deleteFile(path) {
  if (!confirm('Delete ' + path + '?\n(Files are moved to /kl-trash; deleting inside /kl-trash is final.)')) return;
  fetch('/delete?file=' + encodeURIComponent(path), { method: 'DELETE' })
    .then(r => r.text().then(t => { $('msg').textContent = r.ok ? '' : t; refreshFiles(); refreshStatus(); }));
}

$('uploadBtn').onclick = () => {
  const f = $('fileInput').files[0];
  if (!f) return;
  const fd = new FormData();
  fd.append('file', f, f.name);
  $('msg').textContent = 'Uploading...';
  fetch('/upload?path=' + encodeURIComponent(currentPath), { method: 'POST', body: fd })
    .then(r => r.text().then(t => { $('msg').textContent = r.ok ? '' : t; $('fileInput').value = ''; refreshFiles(); refreshStatus(); }))
    .catch(() => { $('msg').textContent = 'Upload failed'; });
};

function reboot() {
  if (!confirm('Reboot the dongle? The roaster loses the stick for a few seconds.')) return;
  fetch('/reboot', { method: 'POST' });
}

updateBreadcrumb();
refreshFiles();
refreshStatus();
setInterval(refreshStatus, 3000);
setInterval(refreshFiles, 30000);
</script>
</body>
</html>
)HTML";
