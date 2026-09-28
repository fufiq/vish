import { defineConfig } from 'electron-vite';

// main - connect to VM (Unix-socket) and run `tz serve`; preload - byte bridge;
// renderer - node as Web Components. The main/preload electron-vite target is the Node
// of the Electron version; the window is esnext, without downlevel.
export default defineConfig({
  main: {},
  preload: {},
  renderer: { build: { target: 'esnext' } },
});
