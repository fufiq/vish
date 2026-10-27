// Main process: byte pipe between window and VM. The protocol is not
// understood here - frames decode the window; VM is the only state.
//
//
// Connect, otherwise start: the editor connects to the socket; if the VM is not there
// and the program is set, it raises the `vish serve <программа> <сокет>` itself (and
// extinguishes when it exits). Several editors can be connected to one VM.
// electron-vite dev -- --program ../tests/stats.vish [--socket path]
import { spawn, type ChildProcess } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { connect, type Socket } from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { app, BrowserWindow, ipcMain } from 'electron';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const socketPath = arg('socket') ?? join(tmpdir(), 'vish-editor.sock');
const program = arg('program');
const vishBin = process.env.VISH_BIN ?? join(app.getAppPath(), '../out/vish');

let vm: ChildProcess | undefined;
let socket: Socket | undefined;
let win: BrowserWindow | undefined;

// Out the window - only while it is alive: the VM socket closes after the window.
function toWindow(channel: string, ...args: unknown[]): void {
  if (win && !win.isDestroyed()) win.webContents.send(channel, ...args);
}

function status(text: string, lost = false): void {
  toWindow('vm:status', text, lost);
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
  status(`запуск VM: ${vishBin} serve ${program} ${socketPath}`);
  vm = spawn(vishBin, ['serve', program, socketPath], {
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
  await attach();
}

// Connect to the VM (if it is not - raise); lost - "Connect" in the bangs of the window.
async function attach(): Promise<void> {
  try {
    socket = await connectVm();
  } catch (e) {
    status(`нет VM: ${(e as Error).message}`, true);
    return;
  }
  status(`VM: ${socketPath}`);
  socket.on('data', (d: Buffer) => {
    toWindow('vm:data', new Uint8Array(d));
  });
  socket.on('close', () => {
    status('VM отключилась', true);
  });
  toWindow('vm:ready');
}

// AI: The window bar drives your Claude Code (`claude -p`; login is your subscription as in VS Code) with the
// vish MCP server - the agent becomes a client of the same VM. Built-in tools are only reading; edits are
// sentences (vish_propose), they are made by a person: the decision is made by a person, the AI only offers.
// The conversation continues in the same session (-resume).
//
const aiSystem = [
  'Ты помощник в редакторе языка vish: язык, VM и семантический редактор над AST.',
  'Программа живёт в VM: элементы и их ключи — vish_items, ошибки проверки с трейсом — vish_check,',
  'запуск main() — vish_run. Менять программу можно только через vish_propose (элемент целиком по',
  'ключу); правку принимает человек в редакторе. Синтаксис бери из текста программы; спецификация —',
  'docs/spec.md репозитория vish, если доступна. Отвечай по-русски, коротко.',
].join(' ');
let aiSession: string | undefined;
let aiProc: ChildProcess | undefined;
function ai(kind: string, text: string): void {
  toWindow('ai:event', kind, text);
}
ipcMain.on('ai:ask', (_e, text: string) => {
  if (aiProc) return;
  const mcp = JSON.stringify({
    mcpServers: {
      vish: {
        command: process.execPath,
        args: [join(import.meta.dirname, 'mcp.js'), '--socket', socketPath],
        env: { ELECTRON_RUN_AS_NODE: '1' },
      },
    },
  });
  const resume = aiSession !== undefined;
  aiSession ??= randomUUID();
  const args = [
    '-p',
    '--output-format',
    'stream-json',
    '--verbose',
    '--strict-mcp-config',
    '--mcp-config',
    mcp,
    '--append-system-prompt',
    aiSystem,
    ...(resume ? ['--resume', aiSession] : ['--session-id', aiSession]),
    '--tools',
    'Read,Grep,Glob',
    '--allowedTools',
    'mcp__vish',
    'Read',
    'Grep',
    'Glob',
  ];
  const proc = spawn(process.env.CLAUDE_BIN ?? 'claude', args, {
    cwd: program ? dirname(program) : process.cwd(),
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  aiProc = proc;
  proc.stdin.end(text); // query - input (a positional argument would be confused with flag lists)
  let err = '';
  let finished = false;
  proc.stderr.on('data', (d: Buffer) => (err += d.toString()));
  createInterface({ input: proc.stdout }).on('line', (line) => {
    let m: {
      type?: string;
      message?: {
        content?: {
          type: string;
          text?: string;
          name?: string;
          input?: unknown;
        }[];
      };
      result?: string;
      is_error?: boolean;
    };
    try {
      m = JSON.parse(line) as typeof m;
    } catch {
      return;
    }
    if (m.type === 'assistant')
      for (const c of m.message?.content ?? []) {
        if (c.type === 'text' && c.text) ai('text', c.text);
        if (c.type === 'tool_use')
          ai(
            'tool',
            `${(c.name ?? '').replace('mcp__vish__', '')} ${JSON.stringify(c.input).slice(0, 160)}`,
          );
      }
    if (m.type === 'result') {
      finished = true;
      ai(
        m.is_error ? 'error' : 'done',
        m.is_error ? (m.result ?? 'ошибка') : '',
      );
    }
  });
  proc.on('error', (e) => {
    finished = true;
    ai(
      'error',
      `не запустился claude: ${e.message} (нужен Claude Code: claude в PATH или CLAUDE_BIN)`,
    );
  });
  proc.on('close', (code) => {
    aiProc = undefined;
    if (!finished)
      ai(
        'error',
        `claude завершился с кодом ${String(code)}\n${err.slice(-800)}`,
      );
  });
});
ipcMain.on('ai:stop', () => {
  aiProc?.kill();
});

ipcMain.on('vm:reconnect', () => {
  if (socket && !socket.destroyed) return;
  void attach();
});

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
