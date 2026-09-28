// Bridge window to the main process: only bytes of frames and the status bar.
import { contextBridge, ipcRenderer } from 'electron';

const api = {
  send(bytes: Uint8Array): void {
    ipcRenderer.send('vm:send', bytes);
  },
  onData(cb: (bytes: Uint8Array) => void): void {
    ipcRenderer.on('vm:data', (_e, bytes: Uint8Array) => {
      cb(bytes);
    });
  },
  onReady(cb: () => void): void {
    ipcRenderer.on('vm:ready', () => {
      cb();
    });
  },
  onStatus(cb: (text: string) => void): void {
    ipcRenderer.on('vm:status', (_e, text: string) => {
      cb(text);
    });
  },
};

export type VmBridge = typeof api;

contextBridge.exposeInMainWorld('vm', api);
