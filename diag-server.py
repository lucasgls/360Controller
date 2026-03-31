#!/usr/bin/env python3
"""Xbox 360 DriverKit — Diagnostic Web Server"""
import http.server, json, subprocess, html

PORT = 8360

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
    return r.stdout.strip() or r.stderr.strip() or "(empty)"

def get_status():
    ioreg = run(["ioreg", "-r", "-n", "XboxReceiverDriver"])
    sysext = run(["systemextensionsctl", "list"])
    ps = run(["ps", "aux"])
    dext = "\n".join(l for l in ps.split("\n")
                     if "XboxReceiver" in l and "grep" not in l and "log stream" not in l and "diag" not in l)
    usb = run(["bash", "-c",
               "ioreg -p IOUSB -l 2>/dev/null | grep -A15 '\"idProduct\" = 681'"])
    logs = run(["log", "show", "--last", "2m",
                "--predicate", 'eventMessage CONTAINS "[XboxReceiver]"',
                "--info", "--debug"])
    return {
        "ioreg": ioreg or "(XboxReceiverDriver not found in IORegistry)",
        "sysext": sysext,
        "process": dext or "(dext process not running)",
        "usb": usb or "(USB device not found — dongle disconnected?)",
        "logs": logs or "(no os_log output captured — normal for DriverKit)",
    }

PAGE = """<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>Xbox 360 Driver Diagnostics</title>
<style>
  * { margin:0; padding:0; box-sizing:border-box; }
  body { background:#1a1a2e; color:#e0e0e0; font:13px/1.5 'SF Mono',monospace; padding:16px; }
  h1 { color:#0f0; font-size:16px; margin-bottom:12px; }
  h2 { color:#4fc3f7; font-size:13px; margin:12px 0 4px; cursor:pointer; }
  h2:hover { color:#81d4fa; }
  .card { background:#16213e; border:1px solid #333; border-radius:6px; padding:10px; margin-bottom:8px; }
  pre { white-space:pre-wrap; word-break:break-all; font-size:12px; max-height:300px; overflow-y:auto; }
  .status { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:6px; }
  .green { background:#0f0; }
  .red { background:#f00; }
  .orange { background:#ffa500; }
  .header { display:flex; align-items:center; gap:12px; margin-bottom:16px; }
  .badge { background:#0a3d0a; color:#0f0; padding:2px 8px; border-radius:4px; font-size:11px; }
  .badge.bad { background:#3d0a0a; color:#f44; }
  #timer { color:#666; font-size:11px; }
</style>
</head><body>
<div class="header">
  <h1>Xbox 360 DriverKit Diagnostics</h1>
  <span id="timer">updating...</span>
</div>
<div id="content">Loading...</div>
<script>
async function refresh() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    const has = (s,k) => s.includes(k);
    const driverActive = has(d.ioreg, 'matched, active');
    const dextRunning = d.process.includes('XboxReceiverDriver');
    const usbConnected = has(d.usb, 'idProduct');
    const sysextOk = has(d.sysext, 'activated enabled');
    const hasLogs = !has(d.logs, 'no os_log');

    document.getElementById('content').innerHTML = `
      <div class="card">
        <b>Quick Status</b><br>
        <span class="status ${driverActive?'green':'red'}"></span> IORegistry: ${driverActive?'matched + active':'NOT active'}<br>
        <span class="status ${dextRunning?'green':'red'}"></span> dext process: ${dextRunning?'running':'NOT running'}<br>
        <span class="status ${usbConnected?'green':'red'}"></span> USB dongle: ${usbConnected?'connected':'disconnected'}<br>
        <span class="status ${sysextOk?'green':'orange'}"></span> sysextd: ${sysextOk?'activated enabled':'check below'}
      </div>
      <div class="card"><h2 onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">IORegistry (XboxReceiverDriver)</h2><pre>${esc(d.ioreg)}</pre></div>
      <div class="card"><h2 onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">System Extensions</h2><pre>${esc(d.sysext)}</pre></div>
      <div class="card"><h2 onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">USB Device</h2><pre>${esc(d.usb)}</pre></div>
      <div class="card"><h2 onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">dext Process</h2><pre>${esc(d.process)}</pre></div>
      <div class="card"><h2 onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">os_log (last 2min)</h2><pre>${esc(d.logs)}</pre></div>
    `;
    document.getElementById('timer').textContent = 'updated ' + new Date().toLocaleTimeString();
  } catch(e) {
    document.getElementById('timer').textContent = 'error: ' + e.message;
  }
}
function esc(s) { const d=document.createElement('div'); d.textContent=s; return d.innerHTML; }
refresh();
setInterval(refresh, 2000);
</script>
</body></html>"""

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(get_status()).encode())
        else:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(PAGE.encode())
    def log_message(self, *a): pass

if __name__ == "__main__":
    print(f"Xbox Diagnostics: http://127.0.0.1:{PORT}")
    http.server.HTTPServer(("127.0.0.1", PORT), H).serve_forever()
