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
  // Lost - communication with VM is lost (Connect button); hint - explanation (Scane “Status”)
  onStatus(cb: (text: string, lost: boolean, hint: string) => void): void {
    ipcRenderer.on(
      'vm:status',
      (_e, text: string, lost?: boolean, hint?: string) => {
        cb(text, lost === true, hint ?? '');
      },
    );
  },
  reconnect(): void {
    ipcRenderer.send('vm:reconnect');
  },
  // AI: query to your Claude Code and its events - text, tool call, end
  aiAsk(text: string): void {
    ipcRenderer.send('ai:ask', text);
  },
  aiStop(): void {
    ipcRenderer.send('ai:stop');
  },
  onAi(cb: (kind: string, text: string) => void): void {
    ipcRenderer.on('ai:event', (_e, kind: string, text: string) => {
      cb(kind, text);
    });
  },
};

export type VmBridge = typeof api;

contextBridge.exposeInMainWorld('vm', api);
