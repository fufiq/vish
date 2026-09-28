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
  // Subscription and question immediately: after rebooting the window, the socket is
  // already connected, and the main process will answer vm:ready again.
  onReady(cb: () => void): void {
    ipcRenderer.on('vm:ready', () => {
      cb();
    });
    ipcRenderer.send('vm:ask');
  },
  onStatus(cb: (text: string) => void): void {
    ipcRenderer.on('vm:status', (_e, text: string) => {
      cb(text);
    });
  },
};

export type VmBridge = typeof api;

contextBridge.exposeInMainWorld('vm', api);
