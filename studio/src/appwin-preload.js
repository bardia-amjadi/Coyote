'use strict';
// Bridge of the "app window" page (where a running or debugged program talks to the user).
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('appwin', {
  onOp: (cb) => { ipcRenderer.on('appwin:op', (_e, p) => cb(p)); },
  event: (type, text) => ipcRenderer.send('appwin:event', { type, text }),
});
