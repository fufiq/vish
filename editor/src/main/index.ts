// Main process: byte pipe between window and VM. The protocol is not
// understood here - frames decode the window; VM is the only state.
//
//
// Connect, otherwise start: the editor connects to the socket; if the VM is not there
// and the program is set, it raises the `tz serve <программа> <сокет>` itself (and
// extinguishes when it exits). Several editors can be connected to one VM.
// electron-vite dev -- --program ../tests/stats.tz [--socket path]
import { spawn, type ChildProcess } from 'node:child_process';
import { connect, type Socket } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { app, BrowserWindow, ipcMain } from 'electron';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const socketPath = arg('socket') ?? join(tmpdir(), 'tz-editor.sock');
const program = arg('program');
const tzBin = process.env.TZ_BIN ?? join(app.getAppPath(), '../out/tz');

let vm: ChildProcess | undefined;
let socket: Socket | undefined;
let win: BrowserWindow | undefined;

// Out the window - only while it is alive: the VM socket closes after the window.
function toWindow(channel: string, ...args: unknown[]): void {
  if (win && !win.isDestroyed()) win.webContents.send(channel, ...args);
}

function status(text: string): void {
  toWindow('vm:status', text);
}

function tryConnect(): Promise<Socket> {
  return new Promise((ok, fail) => {
    const s = connect(socketPath);
    s.once('connect', () => {
      ok(s);
    });
    s.once('error', fail);
  });
}

async function connectVm(): Promise<Socket> {
  try {
    return await tryConnect();
  } catch (e) {
    const code = (e as NodeJS.ErrnoException).code;
    if (!program || (code !== 'ENOENT' && code !== 'ECONNREFUSED')) throw e;
  }
  status(`запуск VM: ${tzBin} serve ${program} ${socketPath}`);
  vm = spawn(tzBin, ['serve', program, socketPath], {
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  for (let i = 0; i < 100; ++i) {
    await new Promise((r) => setTimeout(r, 30));
    try {
      return await tryConnect();
    } catch {
      // socket hasn't been made yet
    }
  }
  throw new Error(`VM не поднялась на ${socketPath}`);
}

async function start(): Promise<void> {
  win = new BrowserWindow({
    width: 1100,
    height: 800,
    webPreferences: {
      preload: join(import.meta.dirname, '../preload/index.mjs'),
      contextIsolation: true,
      sandbox: false, // preload - ES-module
    },
  });
  if (process.env.ELECTRON_RENDERER_URL) {
    await win.loadURL(process.env.ELECTRON_RENDERER_URL);
  } else {
    await win.loadFile(join(import.meta.dirname, '../renderer/index.html'));
  }
  try {
    socket = await connectVm();
  } catch (e) {
    status(`нет VM: ${(e as Error).message}`);
    return;
  }
  status(`VM: ${socketPath}`);
  socket.on('data', (d: Buffer) => {
    toWindow('vm:data', new Uint8Array(d));
  });
  socket.on('close', () => {
    status('VM отключилась');
  });
  toWindow('vm:ready');
}

ipcMain.on('vm:ask', () => {
  if (socket && !socket.destroyed && !socket.connecting) toWindow('vm:ready');
});

ipcMain.on('vm:send', (_e, bytes: Uint8Array) => {
  socket?.write(bytes);
});

app.on('window-all-closed', () => {
  socket?.end();
  vm?.kill();
  app.quit();
});

void app.whenReady().then(start);
