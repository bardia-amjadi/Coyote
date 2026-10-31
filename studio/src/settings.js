'use strict';
// Small JSON settings file (toolchain folder, recent folders, window layout).
const fs = require('fs');
const path = require('path');

const DEFAULTS = { toolchainDir: '', recent: [], theme: 'dark', fontSize: 14, formatOnSave: false, checkOnType: true };

class Settings {
  constructor(file) {
    this.file = file;
    this.data = { ...DEFAULTS };
    try { Object.assign(this.data, JSON.parse(fs.readFileSync(file, 'utf8'))); } catch { /* first run or unreadable: use defaults */ }
  }
  get(k) { return this.data[k]; }
  set(k, v) { this.data[k] = v; this.save(); }
  addRecent(dir) {
    this.data.recent = [dir, ...this.data.recent.filter((d) => d !== dir)].slice(0, 10);
    this.save();
  }
  save() {
    try {
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      fs.writeFileSync(this.file, JSON.stringify(this.data, null, 2));
    } catch { /* settings are best-effort */ }
  }
}

module.exports = { Settings, DEFAULTS };
