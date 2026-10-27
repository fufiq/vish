import { defineConfig } from 'electron-vite';

// main - connect to VM (Unix-socket) and run `vish serve`; preload - byte bridge;
// renderer - node as Web Components. The main/preload electron-vite target is the Node
// of the Electron version; the window is esnext, without downlevel.
export default defineConfig({
  // mcp - MCP-server vish for Claude Code (AI - VM client), run by the Electron binary as Node
  main: {
    build: {
      rollupOptions: {
        input: { index: 'src/main/index.ts', mcp: 'src/mcp/index.ts' },
      },
    },
  },
  preload: {},
  renderer: { build: { target: 'esnext' } },
});
