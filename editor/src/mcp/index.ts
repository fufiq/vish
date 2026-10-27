// MCP server vish. The agent is another VM client: connects to the same socket as the editor, called “AI”
// (seen in the client list), reads elements and check errors, runs the program and edits only sentences -
// the person accepts them. The decision is made by a person, the AI only offers ), so editing the agent
// into the program itself does not go.
//
//
// Transport MCP - stdio, JSON-RPC on the line per message. Claude Code runs the server from
// --mcp-config (the editor's AI panel or your claude/VS Code): node mcp.js --socket path.
import { connect } from 'node:net';
import { createInterface } from 'node:readline';
import { FrameSplitter } from '../protocol/codec';
import {
  decodeServer,
  encodeClient,
  schema,
  type ClientMsg,
  type Items,
  type Output,
  type Proposals,
  type ServerMsg,
  type View,
} from '../protocol/messages';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const socketPath = arg('socket') ?? '/tmp/vish-editor.sock';
const vm = connect(socketPath);
const split = new FrameSplitter();
const send = (m: ClientMsg) => vm.write(encodeClient(m));

// state sent by VM: module representation (for presentation number) and error tag
let view: View | undefined;
let errors: string[] = [];
const waiters: ((m: ServerMsg) => boolean)[] = [];

vm.on('data', (chunk: Buffer) => {
  split.push(new Uint8Array(chunk), (body) => {
    const m = decodeServer(body);
    if (m.type === 's_view' && m.kind === 'module') view = m;
    if (m.type === 's_decor' && m.view === view?.view)
      errors = m.marks.filter((k) => k.kind === 'error').map((k) => k.text);
    for (const [i, w] of [...waiters.entries()].reverse())
      if (w(m)) waiters.splice(i, 1);
  });
});
vm.on('connect', () => {
  send({ type: 'c_hello', version: schema.version });
  send({ type: 'c_name', name: 'ИИ' });
  send({ type: 'c_open_view', kind: 'module' });
});
vm.on('error', (e) => {
  process.stderr.write(`vish mcp: нет VM на ${socketPath}: ${e.message}\n`);
  process.exit(1);
});

// wait for the VM message for which the pick will return the value (not undefined)
function await_<T>(
  pick: (m: ServerMsg) => T | undefined,
  ms = 20000,
): Promise<T> {
  return new Promise((ok, fail) => {
    const timer = setTimeout(() => {
      fail(new Error('VM не ответила'));
    }, ms);
    waiters.push((m) => {
      const v = pick(m);
      if (v === undefined) return false;
      clearTimeout(timer);
      ok(v);
      return true;
    });
  });
}

interface Tool {
  name: string;
  description: string;
  inputSchema: object;
  run: (input: Record<string, unknown>) => Promise<string>;
}

const str = (x: unknown) => (typeof x === 'string' ? x : '');

const tools: Tool[] = [
  {
    name: 'vish_items',
    description:
      'Элементы модуля программы vish: ключ (сигнатура, например area(i64)) и текст каждого. Ключ нужен для vish_propose.',
    inputSchema: {
      type: 'object',
      properties: {},
      additionalProperties: false,
    },
    async run() {
      const got = await_((m) => (m.type === 's_items' ? m : undefined));
      send({ type: 'c_items' });
      const r: Items = await got;
      return r.items
        .map((x) => `${x.key}${x.inner ? ' (внутренний)' : ''}\n${x.text}`)
        .join('\n\n');
    },
  },
  {
    name: 'vish_check',
    description:
      'Ошибки проверки программы (разбор и инстанцирование) с трейсом — последние, что прислала VM. Пусто — ошибок нет.',
    inputSchema: {
      type: 'object',
      properties: {},
      additionalProperties: false,
    },
    run() {
      return Promise.resolve(
        errors.length === 0 ? 'ошибок нет' : errors.join('\n\n'),
      );
    },
  },
  {
    name: 'vish_run',
    description:
      'Запустить main() текущей программы в VM и вернуть вывод (и ошибку исполнения, если была).',
    inputSchema: {
      type: 'object',
      properties: {
        args: { type: 'string', description: 'аргументы через пробел' },
        stdin: { type: 'string', description: 'ввод программы' },
      },
      additionalProperties: false,
    },
    async run(input) {
      let out = '';
      const done = await_((m) => {
        if (m.type !== 's_output') return undefined;
        const o: Output = m;
        out += o.text;
        return o.state === 'running' ? undefined : o;
      }, 30000);
      send({
        type: 'c_run',
        stdin_text: str(input.stdin),
        args: str(input.args),
      });
      try {
        const o = await done;
        return `${o.ok ? 'выполнено' : 'ошибка при исполнении'}\n${out}`;
      } catch {
        send({ type: 'c_stop' });
        return `не закончилась за 30 с — остановлена\n${out}`;
      }
    },
  },
  {
    name: 'vish_propose',
    description:
      'Предложить правку элемента модуля: ключ из vish_items (новый ключ — новый элемент) и его текст целиком. В программу сама не идёт — принимает человек в редакторе.',
    inputSchema: {
      type: 'object',
      properties: {
        key: {
          type: 'string',
          description: 'ключ элемента, например area(i64)',
        },
        text: { type: 'string', description: 'текст элемента целиком' },
        why: { type: 'string', description: 'зачем — одной строкой' },
      },
      required: ['key', 'text', 'why'],
      additionalProperties: false,
    },
    async run(input) {
      const key = str(input.key);
      const text = str(input.text);
      const got = await_((m) => {
        if (m.type !== 's_proposals') return undefined;
        const ps: Proposals = m;
        return ps.proposals.find((p) => p.key === key && p.text === text);
      });
      send({ type: 'c_propose', key, text, why: str(input.why) });
      const p = await got;
      return `предложение #${String(p.id)} (${p.note}) ждёт решения человека в редакторе`;
    },
  },
];

// --- JSON-RPC (MCP stdio) ---------------------------------------------------------------------
interface Request {
  jsonrpc: '2.0';
  id?: number | string;
  method: string;
  params?: Record<string, unknown>;
}

function reply(id: number | string, result: unknown): void {
  process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', id, result })}\n`);
}
function fail(id: number | string, message: string): void {
  process.stdout.write(
    `${JSON.stringify({ jsonrpc: '2.0', id, error: { code: -32601, message } })}\n`,
  );
}

async function handle(r: Request): Promise<void> {
  if (r.id === undefined) return; // notification (initialized, etc.)
  if (r.method === 'initialize') {
    reply(r.id, {
      protocolVersion: str(r.params?.protocolVersion) || '2025-06-18',
      capabilities: { tools: {} },
      serverInfo: { name: 'vish', version: '0.1.0' },
    });
  } else if (r.method === 'tools/list') {
    reply(r.id, {
      tools: tools.map(({ name, description, inputSchema }) => ({
        name,
        description,
        inputSchema,
      })),
    });
  } else if (r.method === 'tools/call') {
    const t = tools.find((x) => x.name === str(r.params?.name));
    if (!t) {
      fail(r.id, `нет инструмента ${str(r.params?.name)}`);
      return;
    }
    try {
      const text = await t.run(
        (r.params?.arguments ?? {}) as Record<string, unknown>,
      );
      reply(r.id, { content: [{ type: 'text', text }] });
    } catch (e) {
      reply(r.id, {
        content: [{ type: 'text', text: String(e) }],
        isError: true,
      });
    }
  } else if (r.method === 'ping') reply(r.id, {});
  else fail(r.id, `нет метода ${r.method}`);
}

createInterface({ input: process.stdin }).on('line', (line) => {
  if (line.trim() === '') return;
  void handle(JSON.parse(line) as Request);
});
