// scripts/web_capture.mjs
//
// Drives the cairns_web WASM page in headless Chrome over the DevTools Protocol
// (raw CDP over a WebSocket -- no Playwright): launch -> navigate -> wait for
// boot -> optionally Runtime.evaluate a scenario -> Page.captureScreenshot.
// Also drains console so engine/[web] logs surface.
//
// Usage: node web_capture.mjs <url> <out.png> [evalJS] [waitMs]
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const URL = process.argv[2] || 'http://localhost:8771/cairns_web.html';
const OUT = process.argv[3] || 'cap.png';
const EVAL = process.argv[4] || '';
const WAIT = parseInt(process.argv[5] || '5000', 10);
const PORT = 9333;
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const chrome = spawn(CHROME, [
  '--headless=new',
  `--remote-debugging-port=${PORT}`,
  '--enable-unsafe-webgpu',
  '--enable-features=Vulkan',
  '--use-angle=metal',
  '--disable-gpu-sandbox',
  '--no-first-run',
  '--no-default-browser-check',
  '--user-data-dir=/tmp/cairns-chrome-profile',
  '--window-size=1300,860',
  'about:blank',
], { stdio: 'ignore' });

class CDP {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = [];
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && this.pending.has(m.id)) {
        this.pending.get(m.id)(m);
        this.pending.delete(m.id);
      } else if (m.method) {
        this.handlers.forEach((h) => h(m));
      }
    };
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((res) => {
      this.pending.set(id, res);
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  on(fn) { this.handlers.push(fn); }
}

(async () => {
  let targets = null;
  for (let i = 0; i < 80; i++) {
    try {
      const res = await fetch(`http://localhost:${PORT}/json`);
      targets = await res.json();
      if (targets.find((t) => t.type === 'page')) break;
    } catch (e) { /* chrome not up yet */ }
    await sleep(100);
  }
  const page = targets.find((t) => t.type === 'page');
  if (!page) { console.log('no page target'); chrome.kill(); process.exit(1); }

  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  const cdp = new CDP(ws);
  const logs = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') {
      logs.push('[console] ' + m.params.args.map((a) => a.value ?? a.description).join(' '));
    } else if (m.method === 'Runtime.exceptionThrown') {
      logs.push('[exception] ' + (m.params.exceptionDetails?.exception?.description || JSON.stringify(m.params.exceptionDetails)));
    }
  });
  cdp.on((m) => {
    if (m.method === 'Log.entryAdded') {
      logs.push('[log:' + m.params.entry.level + '] ' + m.params.entry.text);
    }
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Log.enable');
  await cdp.send('Page.navigate', { url: URL });
  await sleep(WAIT);
  if (EVAL) {
    const r = await cdp.send('Runtime.evaluate', { expression: EVAL, returnByValue: true });
    logs.push('[eval] ' + JSON.stringify(r.result?.result?.value ?? r.result));
    await sleep(2500);
  }
  const shot = await cdp.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
  if (shot.result?.data) {
    writeFileSync(OUT, Buffer.from(shot.result.data, 'base64'));
    console.log('wrote ' + OUT);
  } else {
    console.log('no screenshot: ' + JSON.stringify(shot).slice(0, 300));
  }
  console.log(logs.join('\n'));
  chrome.kill();
  process.exit(0);
})().catch((e) => { console.error(e); chrome.kill(); process.exit(1); });
