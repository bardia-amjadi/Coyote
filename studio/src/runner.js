'use strict';
// Runs external programs with streamed output. Never uses a shell.
const { spawn } = require('child_process');

// Starts `cmd args`. Returns { done: Promise<{code, signal, error}>, kill(), write(text), child }.
function start(cmd, args, { cwd, env, onStdout, onStderr } = {}) {
  let child;
  const done = new Promise((resolve) => {
    try {
      child = spawn(cmd, args, { cwd, env: env ? { ...process.env, ...env } : process.env, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (e) {
      resolve({ code: 127, signal: null, error: e.message });
      return;
    }
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (d) => onStdout && onStdout(d));
    child.stderr.on('data', (d) => onStderr && onStderr(d));
    child.stdin.on('error', () => {});
    child.on('error', (e) => resolve({ code: 127, signal: null, error: e.code === 'ENOENT' ? `cannot start '${cmd}' (not found)` : e.message }));
    child.on('close', (code, signal) => resolve({ code: code === null ? 128 : code, signal, error: null }));
  });
  return {
    done,
    get child() { return child; },
    kill() { if (child && !child.killed) child.kill(); },
    write(text) { if (child && child.stdin.writable) child.stdin.write(text); },
    closeStdin() { if (child && child.stdin.writable) child.stdin.end(); },
  };
}

// Runs to completion and collects output.
async function capture(cmd, args, opts = {}) {
  let stdout = '', stderr = '';
  const p = start(cmd, args, { ...opts, onStdout: (d) => (stdout += d), onStderr: (d) => (stderr += d) });
  const r = await p.done;
  return { ...r, stdout, stderr };
}

module.exports = { start, capture };
