#!/usr/bin/env node
'use strict';
// Version helper. The VERSION file is the single source of truth.
//   node tools/version.js            print the version
//   node tools/version.js sync       write it into studio/package.json and editors/vscode/package.json
//   node tools/version.js check      fail if those files are out of sync
//   node tools/version.js full <n>   print the installer version MAJOR.MINOR.PATCH.<n> (n = CI run number; a fourth VERSION part is not used)
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const version = fs.readFileSync(path.join(root, 'VERSION'), 'utf8').trim();
if (!/^\d+\.\d+\.\d+(\.\d+)?$/.test(version)) { console.error(`VERSION must look like 1.2.3 or 1.2.3.4, got '${version}'`); process.exit(2); }
// A hotfix version (0.6.0.9) has a fourth part. package.json files (npm, vsce) need plain semver and the installer
// appends the CI run number, so those use the first three parts only.
const core = version.split('.').slice(0, 3).join('.');
const targets = ['studio/package.json', 'editors/vscode/package.json'];
const cmd = process.argv[2];

if (!cmd) console.log(version);
else if (cmd === 'full') {
  const n = Number(process.argv[3] || 0);
  if (!Number.isInteger(n) || n < 0 || n > 65535) { console.error('build number must be 0..65535'); process.exit(2); }
  console.log(`${core}.${n}`);
} else if (cmd === 'sync' || cmd === 'check') {
  let bad = 0;
  for (const t of targets) {
    const f = path.join(root, t);
    const j = JSON.parse(fs.readFileSync(f, 'utf8'));
    if (j.version === core) continue;
    if (cmd === 'check') { console.error(`${t} has version ${j.version}, VERSION says ${version} (expects ${core}) (run: node tools/version.js sync)`); bad++; continue; }
    j.version = core;
    fs.writeFileSync(f, JSON.stringify(j, null, 2) + '\n');
    console.log(`updated ${t}`);
  }
  process.exit(bad ? 1 : 0);
} else { console.error('unknown command ' + cmd); process.exit(2); }
