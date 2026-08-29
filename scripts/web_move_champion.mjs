// scripts/web_move_champion.mjs
//
// R2 (windowed click->eval->move) driver: drive the cairns_web WASM page in
// Chrome over the DevTools Protocol and *move a champion* -- the interactive
// loop the headless gate can't prove.
// Sequence: boot -> scn('three_champ') (3 static champions) -> screenshot BEFORE
// -> listEntities -> selection.set(first) -> scene.setTransform(first, shifted)
// -> screenshot AFTER. The two PNGs show the selected champion translated.
//
// Browser clicking isn't wired (single-threaded web, no SDL input), so the
// "click a champion" step is the programmatic equivalent: pick the first entity
// from listEntities and mark it selected. Everything else is the real engine.
//
// Usage: node web_move_champion.mjs [url] [before.png] [after.png]
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const URL = process.argv[2] || 'http://localhost:8771/cairns_web.html';
const BEFORE = process.argv[3] || '/tmp/w7_before.png';
const AFTER = process.argv[4] || '/tmp/w7_after.png';
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
  '--user-data-dir=/tmp/cairns-chrome-w7',
  '--window-size=1300,860',
  'about:blank',
], { stdio: 'ignore' });

class CDP {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = [];
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && this.pending.has(m.id)) { this.pending.get(m.id)(m); this.pending.delete(m.id); }
      else if (m.method) { this.handlers.forEach((h) => h(m)); }
    };
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((res) => { this.pending.set(id, res); this.ws.send(JSON.stringify({ id, method, params })); });
  }
  on(fn) { this.handlers.push(fn); }
}

const evalJs = async (cdp, expr) => {
  const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
  if (r.result?.exceptionDetails) { throw new Error('eval threw: ' + JSON.stringify(r.result.exceptionDetails)); }
  return r.result?.result?.value;
};
const shoot = async (cdp, path) => {
  const s = await cdp.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
  if (!s.result?.data) { throw new Error('no screenshot'); }
  writeFileSync(path, Buffer.from(s.result.data, 'base64'));
};

(async () => {
  let page = null;
  for (let i = 0; i < 80; i++) {
    try {
      const t = await (await fetch(`http://localhost:${PORT}/json`)).json();
      page = t.find((x) => x.type === 'page');
      if (page) break;
    } catch { /* not up */ }
    await sleep(100);
  }
  if (!page) { console.log('no page target'); chrome.kill(); process.exit(1); }

  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  const cdp = new CDP(ws);
  const logs = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') { logs.push('[c] ' + m.params.args.map((a) => a.value ?? a.description).join(' ')); }
    else if (m.method === 'Log.entryAdded') { logs.push('[' + m.params.entry.level + '] ' + m.params.entry.text); }
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Log.enable');
  await cdp.send('Page.navigate', { url: URL });
  await sleep(6000);  // adapter/device callback chain + first frames

  // 1) spawn the three static champions via the shell's scenario helper.
  await evalJs(cdp, `scn("three_champ")`);
  await sleep(2500);
  await shoot(cdp, BEFORE);

  // 2) "select" a champion (listEntities is the no-input click). dispatch()
  // wraps the handler return under .result (mirrors the native golden JS).
  const list = await evalJs(cdp, `JSON.stringify(cairns.dispatch("cairns.scene.listEntities", {}))`);
  const ents = (JSON.parse(list).result || {}).entities || [];
  console.log('entities:', JSON.stringify(ents));
  if (!ents.length) { console.log('NO ENTITIES -- spawn failed'); console.log(logs.slice(-15).join('\n')); chrome.kill(); process.exit(1); }
  const target = ents[Math.floor(ents.length / 2)];  // the middle champion
  await evalJs(cdp, `cairns.dispatch("cairns.selection.set", { targets: [{ type: "entity", id: ${target} }] })`);
  const sel = await evalJs(cdp, `JSON.stringify(cairns.dispatch("cairns.selection.get", {}))`);
  console.log('selection.get:', sel);

  // 3) eval-to-move: translate the selected champion right + toward the camera.
  const moveRes = await evalJs(cdp, `JSON.stringify(cairns.dispatch("cairns.scene.setTransform", { entity: ${target}, x: 2.5, y: 0.0, z: -3.0, scale: 1.0 }))`);
  console.log('setTransform:', moveRes);
  await sleep(2000);
  await shoot(cdp, AFTER);

  console.log(`wrote ${BEFORE} + ${AFTER}; moved entity ${target}`);
  console.log(logs.slice(-6).join('\n'));
  chrome.kill();
  process.exit(0);
})().catch((e) => { console.error('W7 driver error:', e.message); chrome.kill(); process.exit(1); });
