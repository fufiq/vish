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
  type Extensions,
  type Items,
  type Output,
  type ServerMsg,
  type AgentResult,
  type WorkspaceAsk,
  type ContextAsk,
  type CheckAsk,
} from '../protocol/messages';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const socketPath = arg('socket') ?? '/tmp/vish-editor.sock';
const vm = connect(socketPath);
const split = new FrameSplitter();
const send = (m: ClientMsg) => vm.write(encodeClient(m));

// All expectations have a deadline and are deleted upon completion, timeout and loss of VM.
interface Waiter {
  accept: (m: ServerMsg) => boolean;
  fail: (error: Error) => void;
}
const waiters = new Set<Waiter>();
function await_<T>(
  pick: (m: ServerMsg) => T | undefined,
  ms = 20000,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const finish = () => {
      clearTimeout(timer);
      waiters.delete(waiter);
    };
    const waiter: Waiter = {
      accept(m) {
        if (m.type === 's_error') {
          waiter.fail(new Error(m.text));
          return true;
        }
        const result = pick(m);
        if (result === undefined) return false;
        finish();
        resolve(result);
        return true;
      },
      fail(error) {
        finish();
        reject(error);
      },
    };
    const timer = setTimeout(() => {
      waiter.fail(new Error('VM не ответила'));
    }, ms);
    waiters.add(waiter);
  });
}
const ready = await_((m) => (m.type === 's_view' ? true : undefined));
void ready.catch(() => undefined); // The error message will receive a waiting tool/call
vm.on('data', (chunk: Buffer) => {
  split.push(new Uint8Array(chunk), (body) => {
    const message = decodeServer(body);
    for (const waiter of [...waiters]) waiter.accept(message);
  });
});
vm.on('connect', () => {
  send({ type: 'c_hello', version: schema.version });
  send({ type: 'c_name', name: 'ИИ' });
  send({ type: 'c_open_view', kind: 'module' });
});
function disconnected(error: Error): void {
  for (const waiter of [...waiters]) waiter.fail(error);
}
vm.on('error', disconnected);
vm.on('close', () => {
  disconnected(new Error('VM отключилась'));
});
let nextRequest = 0;
async function agentCall(
  message:
    | Omit<ContextAsk, 'request'>
    | Omit<CheckAsk, 'request'>
    | Omit<WorkspaceAsk, 'request'>,
): Promise<AgentResult> {
  const request = ++nextRequest;
  const result = await_((m) =>
    m.type === 's_agent' && m.request === request ? m : undefined,
  );
  send({ ...message, request });
  return result;
}
function agentText(result: AgentResult): string {
  const text = JSON.stringify(result);
  if (!result.ok) throw new Error(text);
  return text;
}
async function workspace(
  work: number,
  action: string,
  fields: Partial<WorkspaceAsk> = {},
): Promise<AgentResult> {
  return agentCall({
    type: 'c_workspace',
    workspace: work,
    action,
    revision: 0,
    key: '',
    text: '',
    args: '',
    input: '',
    expected: '',
    ...fields,
  });
}

interface Tool {
  name: string;
  description: string;
  inputSchema: object;
  run: (input: Record<string, unknown>) => Promise<string>;
}

const str = (x: unknown) => (typeof x === 'string' ? x : '');
const num = (x: unknown) =>
  typeof x === 'number' && Number.isSafeInteger(x) && x >= 0 ? x : 0;

const tools: Tool[] = [
  {
    name: 'vish_items',
    description:
      'Элементы модуля программы vish: ключ (сигнатура, например area(i64)) и текст каждого. Ключ нужен для vish_propose.',
    inputSchema: {
      type: 'object',
      properties: {
        key: {
          type: 'string',
          description: 'Точный ключ элемента, если нужен один',
        },
      },
      additionalProperties: false,
    },
    async run(input) {
      const got = await_((m) => (m.type === 's_items' ? m : undefined));
      send({ type: 'c_items' });
      const r: Items = await got;
      return r.items
        .filter((x) => str(input.key) === '' || x.key === input.key)
        .map((x) => `${x.key}${x.inner ? ' (внутренний)' : ''}\n${x.text}`)
        .join('\n\n');
    },
  },
  {
    name: 'vish_open_module',
    description:
      'Открыть вид другого модуля базы по ключу (подключённый use модуль или модуль репозитория, например src.versions) — корень нового представления; правки в нём ложатся в этот модуль.',
    inputSchema: {
      type: 'object',
      additionalProperties: false,
      required: ['key'],
      properties: { key: { type: 'string', description: 'ключ модуля, как в use' } },
    },
    async run(input) {
      send({ type: 'c_open_module', key: str(input.key) });
      return `вид модуля ${str(input.key)} запрошен`;
    },
  },
  {
    name: 'vish_task',
    description:
      'Задача проекта: внешняя команда из [[editor]] task_<имя>(code) программы vish (например, check — сборка и тесты). Запуск без блокировки VM; вывод — в панель запуска редактора, здесь — только запуск.',
    inputSchema: {
      type: 'object',
      additionalProperties: false,
      required: ['name'],
      properties: { name: { type: 'string', description: 'имя задачи: task_<имя>' } },
    },
    async run(input) {
      send({ type: 'c_task', name: str(input.name) });
      return `задача ${str(input.name)} запущена`;
    },
  },
  {
    name: 'vish_extensions',
    description:
      'Точки расширения редактора в программе vish и её плагинах (модули с [[editor]]-функциями, подключённые use): модуль, имя, сигнатура, вид; «перекрыта» — одноимённую ближе объявлена другая.',
    inputSchema: { type: 'object', additionalProperties: false },
    async run() {
      const got = await_((m) => (m.type === 's_extensions' ? m : undefined));
      send({ type: 'c_extensions' });
      const r: Extensions = await got;
      return r.points.length === 0
        ? 'расширений нет'
        : r.points
            .map(
              (x) =>
                `${x.module === '' ? 'программа' : x.module}: ${x.sig} — ${x.kind}${x.shadowed ? ' (перекрыта)' : ''}`,
            )
            .join('\n');
    },
  },
  {
    name: 'vish_context',
    description:
      'Точный узел AST, его элемент и сигнатура, типы и установленные VM связи вызовов. Возвращает ревизию программы. Не выбирай перегрузку по одному имени.',
    inputSchema: {
      type: 'object',
      properties: {
        node: { type: 'integer', minimum: 0 },
        revision: { type: 'integer', minimum: 0 },
      },
      additionalProperties: false,
    },
    async run(input) {
      return agentText(
        await agentCall({
          type: 'c_context',
          view: 0,
          node: typeof input.node === 'number' ? num(input.node) : 0xffffffff,
          revision: num(input.revision),
        }),
      );
    },
  },
  {
    name: 'vish_check',
    description:
      'Дождаться завершённой проверки текущей программы. Возвращает ревизию и ошибки; ошибка или устаревшая ревизия — isError. Проверяет исходную программу, а предложения проверяет vish_propose в отдельной копии.',
    inputSchema: {
      type: 'object',
      properties: { revision: { type: 'integer', minimum: 0 } },
      additionalProperties: false,
    },
    async run(input) {
      return agentText(
        await agentCall({ type: 'c_check', revision: num(input.revision) }),
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
      'Подготовить проверяемый пакет изменений в копии программы. Нужна ревизия из vish_context/check. changes — ключи исходных элементов и полный новый текст; для одного элемента допустимы key/text. tests — запуски main с точным ожидаемым stdout. Проверка и все заданные тесты должны пройти до предложения человеку. Без тестов это явно отмечено. Каноническая программа не меняется.',
    inputSchema: {
      type: 'object',
      properties: {
        revision: { type: 'integer', minimum: 1 },
        key: { type: 'string' },
        text: { type: 'string' },
        changes: {
          type: 'array',
          minItems: 1,
          maxItems: 32,
          items: {
            type: 'object',
            properties: { key: { type: 'string' }, text: { type: 'string' } },
            required: ['key', 'text'],
            additionalProperties: false,
          },
        },
        tests: {
          type: 'array',
          maxItems: 16,
          items: {
            type: 'object',
            properties: {
              args: { type: 'string' },
              stdin: { type: 'string' },
              expected: { type: 'string' },
            },
            required: ['expected'],
            additionalProperties: false,
          },
        },
        why: { type: 'string' },
      },
      required: ['revision', 'why'],
      additionalProperties: false,
    },
    async run(input) {
      const records = (value: unknown): Record<string, unknown>[] => {
        if (!Array.isArray(value)) return [];
        return value.map((item: unknown) => {
          if (typeof item !== 'object' || item === null || Array.isArray(item))
            throw new Error('нужен объект');
          return item as Record<string, unknown>;
        });
      };
      const changes =
        input.changes === undefined
          ? [{ key: input.key, text: input.text }]
          : records(input.changes);
      const tests = records(input.tests);
      if (
        num(input.revision) === 0 ||
        changes.length === 0 ||
        changes.length > 32 ||
        tests.length > 16
      )
        throw new Error('нужна ревизия, 1–32 правки и не более 16 тестов');
      if (
        changes.some(
          (change) => str(change.key) === '' || str(change.text) === '',
        ) ||
        tests.some((test) => typeof test.expected !== 'string')
      )
        throw new Error('нужны ключи, тексты правок и ожидаемый вывод тестов');
      const created = await workspace(0, 'create', {
        revision: num(input.revision),
      });
      agentText(created);
      let work = created.workspace;
      try {
        for (const change of changes)
          agentText(
            await workspace(work, 'edit', {
              key: str(change.key),
              text: str(change.text),
            }),
          );
        agentText(await workspace(work, 'check'));
        for (const test of tests)
          agentText(
            await workspace(work, 'test', {
              args: str(test.args),
              input: str(test.stdin),
              expected: str(test.expected),
            }),
          );
        const offered = await workspace(work, 'offer', {
          text: str(input.why),
        });
        const text = agentText(offered);
        work = 0;
        return text;
      } finally {
        if (work !== 0) await workspace(work, 'drop').catch(() => undefined);
      }
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
      await ready;
      if (vm.destroyed) throw new Error('VM отключилась');
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

let calls = Promise.resolve();
createInterface({ input: process.stdin }).on('line', (line) => {
  if (line.trim() === '') return;
  const request = JSON.parse(line) as Request;
  // Old items/run do not have a request-id: their answers should not be mixed.
  if (request.method === 'tools/call')
    calls = calls.then(() => handle(request));
  else void handle(request);
});
