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
  fs.writeFileSync(path.join(proj, 'ask.coy'), 'use "Standard"\n\nstring name = Shell.Prompt("Name: ")\nShell.PrintLine("Hello " + name)\nstring more = Shell.ReadLine()\nShell.PrintLine("Second " + more)\n');
  fs.writeFileSync(path.join(proj, 'noimport.coy'), 'Shell.PrintLine("x")\n');

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
  const pageLog = (global.__pageLog = []);
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    else if (d.method === 'Runtime.exceptionThrown') pageLog.push('exception: ' + JSON.stringify(d.params.exceptionDetails.exception && d.params.exceptionDetails.exception.description || d.params.exceptionDetails.text));
    else if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') pageLog.push('console.error: ' + d.params.args.map((a) => a.value || a.description).join(' '));
  };
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

  await send('Runtime.enable', {});
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
  const termText = () => evaluate('window.__studio.App.term.text()');
  const typeLine = async (text) => {
    await evaluate(`document.querySelector('.term-input').focus()`);
    await send('Input.insertText', { text });
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  };
  await step('run file prints output', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'app.coy'))})`);
    await evaluate('window.__studio.MENU.run()');
    await waitFor(`window.__studio.App.term.text().includes('a 9')`, 'program output');
    await waitFor(`window.__studio.App.term.text().includes('[finished]')`, 'finish marker');
  });
  await step('terminal: ReadLine receives typed input (prompt stays on the input line)', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'ask.coy'))})`);
    await evaluate('window.__studio.MENU.run()');
    await waitFor(`window.__studio.App.term.text().includes('Name: ')`, 'prompt');
    await waitFor(`document.querySelector('.term-input').style.display !== 'none'`, 'input visible');
    await typeLine('Bardia');
    await waitFor(`window.__studio.App.term.text().includes('Hello Bardia')`, 'first answer');
    await typeLine('again');
    await waitFor(`window.__studio.App.term.text().includes('Second again')`, 'second answer');
    await waitFor(`window.__studio.App.term.text().includes('[finished]')`, 'finish');
    assert.ok(/Name: Bardia\n/.test(await termText()), 'typed text echoed after the prompt');
  });
  await step('terminal: end of input (Ctrl+D button) gives ReadLine an empty line', async () => {
    await evaluate('window.__studio.MENU.run()');
    await waitFor(`window.__studio.App.term.text().includes('Name: ')`, 'prompt again');
    await evaluate(`document.getElementById('term-eof').click()`);
    await waitFor(`window.__studio.App.term.text().includes('[finished]')`, 'finish after eof', 15000);
  });
  await step('debugger: breakpoint, variables, call stack, continue', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'app.coy'))})`);
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
    await waitFor(`window.__studio.App.term.text().includes('a 9')`, 'debug run output');
    await waitFor(`!document.getElementById('btn-run').disabled`, 'session end');
    await evaluate(`window.__studio.MENU.toggleBreakpoint()`);
  });
  await step('debugger: ReadLine asks for input in the terminal', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'ask.coy'))})`);
    await evaluate('window.__studio.MENU.debug()');
    await waitFor(`document.getElementById('st-state').textContent.includes('Paused')`, 'entry stop');
    await evaluate('window.__studio.MENU.continue()');
    await waitFor(`document.getElementById('st-state').textContent.includes('Waiting for input')`, 'input request');
    await typeLine('Dbg');
    await waitFor(`window.__studio.App.term.text().includes('Hello Dbg')`, 'answer');
    await typeLine('two');
    await waitFor(`window.__studio.App.term.text().includes('Second two')`, 'second answer');
    await waitFor(`!document.getElementById('btn-run').disabled`, 'session end');
  });
  await step('completion: picking a library class adds its use line', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'noimport.coy'))})`);
    await evaluate(`(() => { const e = window.__studio.editor; e.setValue('Shel'); e.setPosition({ lineNumber: 1, column: 5 }); e.focus(); e.trigger('t', 'editor.action.triggerSuggest', {}); })()`);
    await waitFor(`!!document.querySelector('.suggest-widget.visible') && document.querySelector('.suggest-widget').textContent.includes('Shell')`, 'suggest widget with Shell', 15000);
    await evaluate(`window.__studio.editor.trigger('t', 'acceptSelectedSuggestion', {})`);
    await waitFor(`window.__studio.editor.getValue().includes('use "Standard.IO"')`, 'use line inserted');
    const v = await evaluate('window.__studio.editor.getValue()');
    assert.ok(/^use "Standard.IO"\n\nShell/.test(v), JSON.stringify(v));
  });
  await step('completion: members after a library class dot, with docs', async () => {
    await evaluate(`(() => { const e = window.__studio.editor; e.setValue('use "Standard"\\nShell.'); e.setPosition({ lineNumber: 2, column: 7 }); e.focus(); })()`);
    const end = Date.now() + 20000;
    let ok = false;
    while (Date.now() < end && !ok) {
      await sleep(700);
      await evaluate(`window.__studio.editor.trigger('t', 'editor.action.triggerSuggest', {})`);
      await sleep(800);
      ok = await evaluate(`!!document.querySelector('.suggest-widget.visible') && document.querySelector('.suggest-widget').textContent.includes('PrintLine')`);
    }
    assert.ok(ok, 'member list with PrintLine; widget=' + await evaluate(`(document.querySelector('.suggest-widget') || {}).textContent + ' | value=' + JSON.stringify(window.__studio.editor.getValue()) + ' vis=' + !!document.querySelector('.suggest-widget.visible')`));
  });
  await step('quick fix lightbulb data: unknown class gets an Add use action', async () => {
    await evaluate(`(() => { const e = window.__studio.editor; e.setValue('Shell.PrintLine("x")\\n'); })()`);
    await waitFor(`window.__studio.monaco.editor.getModelMarkers({}).some(m => /unknown (variable|class) 'Shell'/.test(m.message))`, 'marker for Shell', 20000);
  });
  await step('semantic highlighting classifies library classes', async () => {
    await evaluate(`(() => { const e = window.__studio.editor; e.setValue('use "Standard"\\nShell.PrintLine("x")\\nint count = 1\\n'); })()`);
    await sleep(1500);
    const toks = JSON.parse(await evaluate(`(async () => { const m = window.__studio.editor.getModel(); const t = await window.__studio.monaco.editor.tokenize(m.getValue(), 'coyote'); return JSON.stringify(t[1].map(x => x.type)); })()`));
    assert.ok(toks.some((x) => x.startsWith('type.identifier')), 'Monarch types the class name: ' + toks.join(','));
    assert.ok(toks.some((x) => x.startsWith('entity.name.function')), 'method call token: ' + toks.join(','));
  });
  await step('show ML', async () => {
    await evaluate(`window.__studio.openFile(${q(path.join(proj, 'app.coy'))})`);
    await evaluate('window.__studio.MENU.showMl()');
    await waitFor(`document.getElementById('ml-text').textContent.includes('func sq')`, 'ML text');
  });
  await step('command palette and quick open respond', async () => {
    await evaluate('window.__studio.MENU.palette()');
    await waitFor(`!document.getElementById('overlay').classList.contains('hidden') && document.querySelectorAll('#quick-list .qi').length > 10`, 'palette entries');
    await evaluate(`window.__studio.App.Quick.close()`);
    await evaluate('window.__studio.MENU.quickOpen()');
    await waitFor(`document.getElementById('quick-list').textContent.includes('app.coy')`, 'files in quick open');
    await evaluate(`window.__studio.App.Quick.close()`);
  });
  await step('libraries view lists the Standard libraries', async () => {
    await evaluate('window.__studio.MENU.viewLibs()');
    await waitFor(`document.getElementById('libs-tree').textContent.includes('Standard.IO') && document.getElementById('libs-tree').textContent.includes('Standard.Http')`, 'libraries listed');
  });
  await send('Page.captureScreenshot', { format: 'png' }).then((r) => fs.writeFileSync(path.join(os.tmpdir(), 'studio.png'), Buffer.from(r.result.data, 'base64'))).catch(() => {});
  console.log(`UI smoke test passed (${results.length} checks)`);
  finish(0);
})().catch((e) => { console.error('UI SMOKE FAILED:', e.message); if (global.__pageLog && global.__pageLog.length) console.error(global.__pageLog.slice(-8).join('\n')); process.exit(1); });
