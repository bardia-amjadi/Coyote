'use strict';
// The Terminal panel: program output plus an inline input, so `ReadLine` can receive what the user types.
// Text a program prints without a line break (a prompt) stays on the same line as the cursor.
(function () {
  const LINK_RE = /([^\s:"'()<>][^:\n"'<>]*\.coy):(\d+)(?::(\d+))?/g;
  const ANSI_RE = /\x1b\[[0-9;?]*[ -/]*[@-~]/g;
  const MAX_CHARS = 1500000;

  class Terminal {
    // handlers: { onLine(text), onEof(), onInterrupt(), onLink(file, line, col) }
    constructor(host, handlers) {
      this.host = host;
      this.h = handlers;
      this.active = false;
      this.pending = [];
      this.scheduled = false;
      this.chars = 0;
      this.history = [];
      this.histPos = 0;
      this.screen = document.createElement('span');
      this.input = document.createElement('input');
      this.input.className = 'term-input';
      this.input.spellcheck = false;
      this.input.autocomplete = 'off';
      this.input.setAttribute('aria-label', 'Program input');
      this.input.style.display = 'none';
      host.append(this.screen, this.input);
      host.addEventListener('mousedown', (e) => {
        // a plain click focuses the input; selecting text with the mouse must keep working
        if (e.target === this.input) return;
        setTimeout(() => { if (this.active && !String(getSelection()).length) this.input.focus(); }, 0);
      });
      this.input.addEventListener('input', () => this.fit());
      this.input.addEventListener('keydown', (e) => this.key(e));
      this.input.addEventListener('paste', (e) => this.paste(e));
    }

    fit() { this.input.style.width = Math.max(1, this.input.value.length + 1) + 'ch'; }

    key(e) {
      if (e.key === 'Enter') {
        e.preventDefault();
        this.submit(this.input.value);
        this.input.value = '';
        this.fit();
      } else if (e.ctrlKey && (e.key === 'd' || e.key === 'D') && this.input.value === '') {
        e.preventDefault();
        this.h.onEof && this.h.onEof();
      } else if (e.ctrlKey && (e.key === 'c' || e.key === 'C') && this.input.selectionStart === this.input.selectionEnd) {
        e.preventDefault();
        this.h.onInterrupt && this.h.onInterrupt();
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        if (this.histPos > 0) { this.histPos--; this.input.value = this.history[this.histPos]; this.fit(); }
      } else if (e.key === 'ArrowDown') {
        e.preventDefault();
        if (this.histPos < this.history.length) { this.histPos++; this.input.value = this.history[this.histPos] || ''; this.fit(); }
      }
    }

    paste(e) {
      const text = (e.clipboardData || window.clipboardData).getData('text');
      if (!/[\r\n]/.test(text)) return;
      e.preventDefault();
      const parts = (this.input.value + text).split(/\r?\n/);
      const rest = parts.pop();
      for (const p of parts) this.submit(p);
      this.input.value = rest;
      this.fit();
    }

    submit(line) {
      this.flush();
      if (line !== '') { this.history.push(line); }
      this.histPos = this.history.length;
      this.append(line + '\n', 'stdin');
      this.flush();
      this.h.onLine && this.h.onLine(line);
    }

    // The input is only shown while a program runs.
    setActive(on) {
      this.active = on;
      this.input.style.display = on ? '' : 'none';
      this.input.disabled = !on;
      if (!on) this.input.value = '';
      this.fit();
    }
    setWaiting(on) { this.host.classList.toggle('term-waiting', !!on); }
    focus() { if (this.active) this.input.focus({ preventScroll: true }); }

    clear() {
      this.pending = [];
      this.screen.textContent = '';
      this.chars = 0;
    }

    // Queues text; the DOM is updated once per frame so a chatty program does not freeze the window.
    write(text, cls) { this.append(text, cls); }
    writeLine(text, cls) { this.append(text + '\n', cls); }
    append(text, cls) {
      text = String(text).replace(ANSI_RE, '').replace(/\r\n?/g, '\n');
      if (!text) return;
      this.pending.push({ text, cls: cls || '' });
      if (!this.scheduled) {
        this.scheduled = true;
        (window.requestAnimationFrame || setTimeout)(() => { this.scheduled = false; this.flush(); });
      }
    }

    flush() {
      if (!this.pending.length) return;
      const items = this.pending;
      this.pending = [];
      const atEnd = this.host.scrollTop + this.host.clientHeight >= this.host.scrollHeight - 30;
      for (const it of items) {
        const last = this.screen.lastChild;
        const hasLinks = /\.coy:\d/.test(it.text);
        if (!hasLinks && last && last.nodeType === 1 && last.className === it.cls && last.lastChild && last.lastChild.nodeType === 3) {
          last.lastChild.appendData(it.text);
        } else {
          const span = document.createElement('span');
          span.className = it.cls;
          let at = 0, m;
          LINK_RE.lastIndex = 0;
          while ((m = LINK_RE.exec(it.text))) {
            if (m.index > at) span.append(document.createTextNode(it.text.slice(at, m.index)));
            const a = document.createElement('a');
            a.textContent = m[0];
            const file = m[1], line = +m[2], col = m[3] ? +m[3] : 1;
            a.onclick = () => this.h.onLink && this.h.onLink(file, line, col);
            span.append(a);
            at = m.index + m[0].length;
          }
          if (at < it.text.length || !span.firstChild) span.append(document.createTextNode(it.text.slice(at)));
          this.screen.append(span);
        }
        this.chars += it.text.length;
      }
      while (this.chars > MAX_CHARS && this.screen.firstChild) {
        this.chars -= this.screen.firstChild.textContent.length;
        this.screen.removeChild(this.screen.firstChild);
      }
      if (atEnd) this.host.scrollTop = this.host.scrollHeight;
    }

    text() { this.flush(); return this.screen.textContent; }
  }

  window.Terminal = Terminal;
})();
