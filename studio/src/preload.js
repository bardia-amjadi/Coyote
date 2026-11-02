'use strict';
// The only bridge between the page and the main process.
const { contextBridge, ipcRenderer } = require('electron');

const invoke = (ch) => (...args) => ipcRenderer.invoke(ch, ...args);
const on = (ch) => (cb) => { const f = (_e, p) => cb(p); ipcRenderer.on(ch, f); return () => ipcRenderer.removeListener(ch, f); };

contextBridge.exposeInMainWorld('studio', {
  info: invoke('app:info'),
  setSetting: invoke('app:setSetting'),
  versions: invoke('app:versions'),
  doctor: invoke('app:doctor'),
  dialog: { openFolder: invoke('dialog:openFolder'), openFile: invoke('dialog:openFile'), saveAs: invoke('dialog:saveAs'), saveExe: invoke('dialog:saveExe'), confirm: invoke('dialog:confirm') },
  fs: { list: invoke('fs:list'), read: invoke('fs:read'), write: invoke('fs:write'), create: invoke('fs:create'), mkdir: invoke('fs:mkdir'), rename: invoke('fs:rename'), remove: invoke('fs:delete'), reveal: invoke('fs:reveal') },
  project: { open: invoke('project:open'), root: invoke('project:root'), create: invoke('project:new') },
  lib: { pack: invoke('lib:pack') },
  lang: { libs: invoke('lang:libs') },
  search: { files: invoke('fs:files'), text: invoke('fs:search') },
  shell: { openPath: invoke('shell:openPath'), openExternal: invoke('shell:openExternal') },
  tool: { check: invoke('tool:check'), checkText: invoke('tool:checkText'), formatText: invoke('tool:formatText'), format: invoke('tool:format'), showMl: invoke('tool:showMl'), native: invoke('tool:native') },
  run: { file: invoke('run:file'), workflow: invoke('run:workflow'), cpm: invoke('run:cpm'), stdin: invoke('run:stdin'), closeStdin: invoke('run:closeStdin'), stop: invoke('run:stop') },
  debug: {
    start: invoke('debug:start'), breakpoint: invoke('debug:break'), unbreak: invoke('debug:unbreak'), resume: invoke('debug:continue'),
    stepOver: invoke('debug:stepOver'), stepInto: invoke('debug:stepInto'), stepOut: invoke('debug:stepOut'),
    stack: invoke('debug:stack'), vars: invoke('debug:vars'), globals: invoke('debug:globals'),
  },
  onMenu: on('menu'),
  onOpenPath: on('open-path'),
  takeOpenPath: invoke('app:takeOpenPath'),
  onRunOut: on('run:out'),
  onRunErr: on('run:err'),
  onRunExit: on('run:exit'),
  onDebug: on('debug:event'),
});
