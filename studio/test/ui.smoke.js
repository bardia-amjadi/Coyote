'use strict';
// End-to-end UI test: launches the real Electron app (needs a display, e.g. xvfb-run) against a built toolchain and
// drives it over the Chrome DevTools protocol (Node 22 WebSocket; no extra packages).
// Run with: COYOTE_BIN=<dir with coyote,pvm,...> xvfb-run -a node test/ui.smoke.js
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const assert = require('assert');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function getJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, { agent: false }, (res) => { let b = ''; res.on('data', (d) => (b += d)); res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } }); }).on('error', reject);
  });
}

(async () => {
  const appEnv = { ...process.env };
  delete appEnv.NODE_OPTIONS;  // some proxies set options Electron rejects
  const proj = fs.mkdtempSync(path.join(os.tmpdir(), 'studio-ui-'));
  const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'studio-ud-'));
  fs.writeFileSync(path.join(proj, 'packages.json'), '{ "schemaVersion": 1, "name": "ui" }\n');
  fs.writeFileSync(path.join(proj, 'app.coy'), 'hand:con println(string text)\nmethod sq(int n): int {\n    int r = n * n\n    return r\n}\nint a = sq(3)\nprintln("a " + a)\n');
  fs.writeFileSync(path.join(proj, 'bad.coy'), 'hand:con println(string text)\nprintl("x")\n');

  const port = 9300 + Math.floor(Math.random() * 500);
  const child = spawn(require('electron'), [path.join(__dirname, '..'), path.join(proj, 'app.coy'), `--user-data-dir=${userData}`, '--no-sandbox', '--disable-gpu', `--remote-debugging-port=${port}`], { env: appEnv, stdio: 'ignore' });
  const finish = (code) => { try { child.kill(); } catch {} process.exit(code); };
  setTimeout(() => { console.error('UI SMOKE TIMEOUT'); finish(1); }, 140000).unref();

  let target;
  for (let i = 0; i < 80 && !target; i++) {
    try { target = (await getJson(`http://127.0.0.1:${port}/json`)).find((t) => t.type === 'page' && t.url.includes('index.html')); } catch { /* not up yet */ }
    if (!target) await sleep(500);
  }
  if (!target) { console.error('window not found'); finish(1); }

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } };
  const send = (method, params) => new Promise((res) => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const evaluate = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.result.exceptionDetails) throw new Error('page error: ' + JSON.stringify(r.result.exceptionDetails.exception || r.result.exceptionDetails));
    return r.result.result.value;
  };
  const waitFor = async (expr, what, ms = 25000) => {
    const end = Date.now() + ms;
    while (Date.now() < end) { try { if (await evaluate(expr)) return; } catch { /* page still loading */ } await sleep(200); }
    throw new Error('timed out waiting for ' + what);
  };

  const results = [];
  const step = async (name, fn) => { await fn(); results.push(name); console.log('ok -', name); };
  const q = JSON.stringify;

  await waitFor('!!(window.__studio && window.__studio.editor)', 'editor to load', 40000);
  await step('toolchain found', async () => {
    const info = JSON.parse(await evaluate('window.studio.info().then(JSON.stringify)'));
    assert.ok(info.toolchain.dir, 'toolchain dir');
    assert.deepStrictEqual(info.toolchain.missing, []);
  });
  await step('file given on the command line opens in the editor', async () => {
    await waitFor(`window.__studio.editor.getValue().includes('sq(3)')`, 'argv file in editor');
  });
  await step('open project and entry file', async () => {
    await evaluate(`window.__studio.openFolderPath(${q(proj)})`);
    await waitFor(`window.__studio.editor.getValue().includes('sq(3)')`, 'entry file in editor');
  });
  await step('diagnostics shown for a broken file', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'bad.coy'))})`);
    await waitFor(`window.__studio.monaco.editor.getModelMarkers({}).some(m => m.message.includes("unknown function 'printl'"))`, 'diagnostic marker');
  });
  await step('run file prints output', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'app.coy'))})`);
    await evaluate('window.__studio.MENU.run()');
    await waitFor(`document.getElementById('tab-output').textContent.includes('a 9')`, 'program output');
    await waitFor(`document.getElementById('tab-output').textContent.includes('[finished]')`, 'finish marker');
  });
  await step('debugger: breakpoint, variables, call stack, continue', async () => {
    await evaluate(`(() => { window.__studio.editor.setPosition({ lineNumber: 4, column: 1 }); window.__studio.MENU.toggleBreakpoint(); })()`);
    await evaluate('window.__studio.MENU.debug()');
    await waitFor(`document.getElementById('st-state').textContent.includes('Paused')`, 'entry stop');
    await evaluate('window.__studio.MENU.continue()');
    await waitFor(`/app\\.coy:4 \\(breakpoint\\)/.test(document.getElementById('st-state').textContent)`, 'breakpoint stop');
    await waitFor(`document.getElementById('vars').textContent.includes('9') && document.getElementById('vars').textContent.includes('n int')`, 'variables');
    const stack = await evaluate(`document.getElementById('stack').textContent`);
    assert.ok(stack.includes('sq') && stack.includes('$main'), stack);
    await evaluate('window.__studio.MENU.stepOver()');
    await waitFor(`/Paused at app\\.coy:/.test(document.getElementById('st-state').textContent) && !/:4 /.test(document.getElementById('st-state').textContent)`, 'step stop');
    await evaluate('window.__studio.MENU.continue()');
    await waitFor(`document.getElementById('tab-output').textContent.split('a 9').length > 2`, 'second run output');
    await waitFor(`!document.getElementById('btn-run').disabled`, 'session end');
  });
  await step('show ML', async () => {
    await evaluate('window.__studio.MENU.showMl()');
    await waitFor(`document.getElementById('ml-text').textContent.includes('func sq')`, 'ML text');
  });
  await send('Page.captureScreenshot', { format: 'png' }).then((r) => fs.writeFileSync(path.join(os.tmpdir(), 'studio.png'), Buffer.from(r.result.data, 'base64'))).catch(() => {});
  console.log(`UI smoke test passed (${results.length} checks)`);
  finish(0);
})().catch((e) => { console.error('UI SMOKE FAILED:', e.message); process.exit(1); });
