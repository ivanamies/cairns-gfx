// scripts/web_click_scenario.mjs
//
// Verifies the in-canvas imgui scenario picker is *clickable* in the browser
// (DOM mouse -> ImGui IO -> launcher.pending -> scene reload), not just visible.
// Dispatches a real CDP mouse press/release at a page coordinate over a button,
// waits, then screenshots. This is the browser analogue of clicking a button in
// the native metal/vk windowed app.
//
// Usage: node web_click_scenario.mjs <url> <out.png> <pageX> <pageY> [waitMs]
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const URL = process.argv[2] || 'http://localhost:8772/cairns_web.html';
const OUT = process.argv[3] || 'click.png';
const X = parseFloat(process.argv[4] || '170');
const Y = parseFloat(process.argv[5] || '343');
const WAIT = parseInt(process.argv[6] || '6000', 10);
const PORT = 9334;
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
  '--user-data-dir=/tmp/cairns-chrome-click',
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

async function click(cdp, x, y) {
  const base = { x, y, button: 'left', clickCount: 1, buttons: 1 };
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...base, buttons: 0 });
  await sleep(60);
  await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...base });
  await sleep(60);
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...base });
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
      logs.push('[exception] ' + (m.params.exceptionDetails?.exception?.description || ''));
    }
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Page.navigate', { url: URL });
  await sleep(WAIT);
  await click(cdp, X, Y);
  await sleep(3000);
  const shot = await cdp.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
  if (shot.result?.data) {
    writeFileSync(OUT, Buffer.from(shot.result.data, 'base64'));
    console.log('wrote ' + OUT);
  } else {
    console.log('no screenshot: ' + JSON.stringify(shot).slice(0, 300));
  }
  // Surface only the post-click scene state lines.
  console.log(logs.filter((l) => /entities=|draws |GLBs|STEADY|exception/.test(l)).slice(-12).join('\n'));
  chrome.kill();
  process.exit(0);
})().catch((e) => { console.error(e); chrome.kill(); process.exit(1); });
