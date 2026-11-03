'use strict';
/* global appwin, Terminal */
// The separate window a program runs in. Studio mirrors the program's output here and receives what is typed.
const $ = (id) => document.getElementById(id);
const term = new Terminal($('pane-terminal'), {
  onLine: (t) => appwin.event('line', t),
  onEof: () => appwin.event('eof'),
  onInterrupt: () => appwin.event('stop'),
});
$('aw-eof').onclick = () => appwin.event('eof');
$('aw-stop').onclick = () => appwin.event('stop');
const q = new URLSearchParams(location.search);
document.body.className = q.get('theme') === 'light' ? 'theme-light' : 'theme-dark';
appwin.onOp(({ op, a, b }) => {
  if (op === 'write') term.write(a, b);
  else if (op === 'writeLine') term.writeLine(a, b);
  else if (op === 'clear') term.clear();
  else if (op === 'active') { term.setActive(!!a); $('aw-state').className = a ? 'on' + (b === 'debug' ? ' debug' : '') : ''; if (a) term.focus(); }
  else if (op === 'state') { $('aw-state').textContent = a || ''; }
  else if (op === 'waiting') { term.setWaiting(!!a); if (a) term.focus(); }
  else if (op === 'title') { $('aw-title').textContent = a; document.title = a + ' - Coyote App'; }
  else if (op === 'theme') document.body.className = a === 'light' ? 'theme-light' : 'theme-dark';
});
document.addEventListener('mousedown', () => setTimeout(() => term.focus(), 0));
