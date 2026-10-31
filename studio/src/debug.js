'use strict';
// A debugging session: compiles a .coy file, runs `pvm --debug` on it and exposes the line debugger
// (docs/debugging.md) as methods and events.
const EventEmitter = require('events');
const fs = require('fs');
const os = require('os');
const path = require('path');
const runner = require('./runner');

class DebugSession extends EventEmitter {
  constructor(toolchain) {
    super();
    this.tc = toolchain;
    this.proc = null;
    this.buf = '';
    this.waiters = new Map();  // event name -> [resolve]
    this.exited = false;
    this.tmp = null;
  }

  // Emits: 'output' (text), 'stopped' ({reason,file,line,function}), 'runtimeError', 'exited' ({code}), 'log' (text from the compiler/pvm stderr).
  async start(file, { cwd, breakpoints = [] } = {}) {
    this.tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'coyote-debug-'));
    const ml = path.join(this.tmp, 'program.ml');
    const build = await runner.capture(this.tc.tools.coyote, ['build', file, '-o', ml], { cwd });
    if (build.code !== 0) {
      this._cleanup();
      const err = new Error('compile failed');
      err.compileOutput = (build.stderr || build.stdout || build.error || '').trim();
      throw err;
    }
    this.proc = runner.start(this.tc.tools.pvm, ['--debug', ml], { cwd, onStdout: (d) => this._data(d), onStderr: (d) => this.emit('log', d) });
    this.proc.done.then((r) => {
      if (!this.exited) { this.exited = true; this.emit('exited', { code: r.code, error: r.error }); }
      this._flushWaiters();
      this._cleanup();
    });
    for (const b of breakpoints) this.setBreakpoint(b.file, b.line);
  }

  _cleanup() {
    if (this.tmp) { try { fs.rmSync(this.tmp, { recursive: true, force: true }); } catch {} this.tmp = null; }
  }

  _data(chunk) {
    this.buf += chunk;
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      let ev;
      try { ev = JSON.parse(line); } catch { this.emit('log', line + '\n'); continue; }
      this._event(ev);
    }
  }

  _event(ev) {
    const q = this.waiters.get(ev.event);
    if (q && q.length) q.shift()(ev);
    switch (ev.event) {
      case 'output': this.emit('output', ev.text); break;
      case 'stopped': this.emit('stopped', ev); break;
      case 'runtimeError': this.emit('runtimeError', ev); break;
      case 'exited': this.exited = true; this.emit('exited', { code: ev.code }); break;
      default: break;
    }
  }

  _flushWaiters() {
    for (const q of this.waiters.values()) while (q.length) q.shift()(null);
    this.waiters.clear();
  }

  _send(cmd) { if (this.proc && !this.exited) this.proc.write(cmd + '\n'); }

  _request(cmd, eventName) {
    return new Promise((resolve) => {
      if (this.exited || !this.proc) return resolve(null);
      if (!this.waiters.has(eventName)) this.waiters.set(eventName, []);
      this.waiters.get(eventName).push(resolve);
      this._send(cmd);
    });
  }

  setBreakpoint(file, line) { return this._request(`break ${file} ${line}`, 'breakpoint'); }
  clearBreakpoint(file, line) { return this._request(`unbreak ${file} ${line}`, 'breakpoint'); }
  clearBreakpoints() { return this._request('clearbreaks', 'breakpoints-cleared'); }
  resume() { this._send('continue'); }
  stepInto() { this._send('step'); }
  stepOver() { this._send('next'); }
  stepOut() { this._send('out'); }
  async stack() { const e = await this._request('stack', 'stack'); return e ? e.frames : []; }
  async variables(frame = 0) { const e = await this._request(`vars ${frame}`, 'vars'); return e ? e.vars : []; }
  async globals() { const e = await this._request('globals', 'globals'); return e ? e.vars : []; }
  stop() { if (this.proc && !this.exited) { this._send('quit'); setTimeout(() => this.proc && this.proc.kill(), 500).unref(); } }
}

module.exports = { DebugSession };
