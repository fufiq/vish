// @vitest-environment node Full way: MCP stdio → socket → copy of VM → checks → suggestion → human
// acceptance.
import {
  execFileSync,
  spawn,
  type ChildProcessWithoutNullStreams,
} from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { connect } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { beforeAll, describe, expect, it } from 'vitest';
import { FrameSplitter } from '../protocol/codec';
import {
  decodeServer,
  encodeClient,
  schema,
  type AgentResult,
  type ServerMsg,
} from '../protocol/messages';

const root = join(import.meta.dirname, '../../..');
const editor = join(root, 'editor');
beforeAll(() => {
  execFileSync(
    process.execPath,
    [join(editor, 'node_modules/electron-vite/bin/electron-vite.js'), 'build'],
    { cwd: editor, stdio: 'pipe' },
  );
}, 20000);

interface RpcResult {
  content: { type: string; text: string }[];
  isError?: boolean;
}
interface RpcReply {
  id: number;
  result?: RpcResult;
  error?: { message: string };
}
function rpc(client: ChildProcessWithoutNullStreams) {
  let id = 0;
  const pending = new Map<
    number,
    {
      resolve: (result: RpcResult) => void;
      reject: (error: Error) => void;
      timer: ReturnType<typeof setTimeout>;
    }
  >();
  createInterface({ input: client.stdout }).on('line', (line) => {
    const response = JSON.parse(line) as RpcReply;
    const waiter = pending.get(response.id);
    if (!waiter) return;
    clearTimeout(waiter.timer);
    pending.delete(response.id);
    if (response.result) waiter.resolve(response.result);
    else waiter.reject(new Error(response.error?.message ?? 'нет ответа MCP'));
  });
  client.on('exit', () => {
    for (const waiter of pending.values()) {
      clearTimeout(waiter.timer);
      waiter.reject(new Error('MCP завершился'));
    }
    pending.clear();
  });
  return (method: string, params: object = {}) =>
    new Promise<RpcResult>((resolve, reject) => {
      const request = ++id;
      const timer = setTimeout(() => {
        pending.delete(request);
        reject(new Error('MCP не ответил'));
      }, 10000);
      pending.set(request, { resolve, reject, timer });
      client.stdin.write(
        `${JSON.stringify({ jsonrpc: '2.0', id: request, method, params })}\n`,
      );
    });
}
const textOf = (result: RpcResult) =>
  result.content.map((item) => item.text).join('\n');
const agentOf = (result: RpcResult) =>
  JSON.parse(textOf(result)) as AgentResult;

describe('ИИ через настоящий MCP', () => {
  it('проверяет предложение до принятия, отклоняет ошибки и сохраняет исходную программу', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'vish-agent-'));
    const socket = join(dir, 'vm.sock');
    const vm = spawn(
      join(root, 'out/vish'),
      ['serve', join(root, 'tests/editor_proposals.vish'), socket],
      { env: { ...process.env, VISH_GRANT: 'fs,net' } },
    );
    let client: ChildProcessWithoutNullStreams | undefined;
    let human: ReturnType<typeof connect> | undefined;
    try {
      for (let attempt = 0; attempt < 250 && !existsSync(socket); ++attempt)
        await new Promise((resolve) => setTimeout(resolve, 20));
      expect(existsSync(socket)).toBe(true);
      client = spawn(process.execPath, [
        join(editor, 'out/main/mcp.js'),
        '--socket',
        socket,
      ]);
      const call = rpc(client);
      const tool = (name: string, args: object = {}) =>
        call('tools/call', { name, arguments: args });
      // tools/call immediately after the connection: should wait for the ready, not return the empty cache.
      const checked = await tool('vish_check');
      expect(checked.isError).not.toBe(true);
      const initial = agentOf(checked);
      expect(initial.revision).toBeGreaterThan(0);
      expect(initial.text).toContain('проверка завершена');
      const context = agentOf(await tool('vish_context'));
      expect(context.text).toContain('зависимости по выбранным вызовам');
      expect(context.text).toContain('элемент модуль программы');
      expect(context.revision).toBe(initial.revision);

      const invalid = await tool('vish_propose', {
        revision: initial.revision,
        key: 'f(_)',
        text: 'f(x) => missing;',
        why: 'ошибка',
      });
      expect(invalid.isError).toBe(true);
      expect(textOf(invalid)).toContain('неизвестное имя missing');
      const wrongTest = await tool('vish_propose', {
        revision: initial.revision,
        key: 'f(_)',
        text: 'f(x) => x + 3;',
        why: 'ошибка теста',
        tests: [{ expected: '99\n' }],
      });
      expect(wrongTest.isError).toBe(true);
      expect(textOf(wrongTest)).toContain('тест 1: не прошёл');
      const malformed = await tool('vish_propose', {
        revision: initial.revision,
        key: 'f(_)',
        text: 'f(x) => x + 3; another() => 0;',
        why: 'два элемента',
      });
      expect(malformed.isError).toBe(true);
      expect(textOf(malformed)).toContain('один элемент');
      const forbiddenFile = join(dir, 'must-not-exist');
      const effect = await tool('vish_propose', {
        revision: initial.revision,
        key: 'main()',
        text: `main() => create("${forbiddenFile}");`,
        why: 'проверка устройств',
        tests: [{ expected: '' }],
      });
      expect(effect.isError).toBe(true);
      expect(textOf(effect)).toContain('файловая система программе не дана');
      expect(existsSync(forbiddenFile)).toBe(false);
      const state = await tool('vish_propose', {
        revision: initial.revision,
        changes: [
          { key: 'runs', text: '[[state]] runs = 0;' },
          { key: 'main()', text: 'main() => print(runs);' },
        ],
        why: 'проба постоянного состояния',
        tests: [{ expected: '0\n' }],
      });
      expect(state.isError).toBe(true);
      expect(textOf(state)).toContain('постоянное состояние недоступно');

      expect(textOf(await tool('vish_items', { key: 'f(_)' }))).toContain(
        'x + 1',
      );

      const proposed = await tool('vish_propose', {
        revision: initial.revision,
        changes: [
          { key: 'f(_)', text: 'f(x) => x + 3;' },
          { key: 'main()', text: 'main() => print(f(4));' },
        ],
        tests: [{ expected: '7\n' }],
        why: 'согласованная правка',
      });
      expect(proposed.isError).not.toBe(true);
      const prepared = agentOf(proposed);
      expect(prepared.proposal).toBe(1);
      expect(prepared.text).toContain('тест 1: прошёл');
      expect(textOf(await tool('vish_items', { key: 'f(_)' }))).toContain(
        'x + 1',
      );

      human = connect(socket);
      const messages: ServerMsg[] = [];
      const split = new FrameSplitter();
      human.on('data', (chunk: Buffer) => {
        split.push(new Uint8Array(chunk), (body) =>
          messages.push(decodeServer(body)),
        );
      });
      await new Promise<void>((resolve, reject) => {
        human?.once('connect', resolve);
        human?.once('error', reject);
      });
      human.write(encodeClient({ type: 'c_hello', version: schema.version }));
      human.write(encodeClient({ type: 'c_open_view', kind: 'module' }));
      human.write(
        encodeClient({ type: 'c_decide', id: prepared.proposal, accept: true }),
      );
      for (
        let attempt = 0;
        attempt < 250 &&
        !messages.some(
          (m) => m.type === 's_status' && m.text.startsWith('принято:'),
        );
        ++attempt
      )
        await new Promise((resolve) => setTimeout(resolve, 20));
      expect(
        messages.some(
          (m) => m.type === 's_status' && m.text.startsWith('принято:'),
        ),
      ).toBe(true);
      expect(textOf(await tool('vish_items', { key: 'f(_)' }))).toContain(
        'x + 3',
      );
      expect(textOf(await tool('vish_items', { key: 'main()' }))).toContain(
        'f(4)',
      );
      const stale = await tool('vish_check', { revision: initial.revision });
      expect(stale.isError).toBe(true);
      expect(textOf(stale)).toContain('ревизия изменилась');
      const current = agentOf(await tool('vish_check'));
      expect(current.revision).toBeGreaterThan(initial.revision);
    } finally {
      human?.destroy();
      client?.kill();
      vm.kill();
      await Promise.all(
        [vm, client]
          .filter((process) => process !== undefined)
          .map((process) =>
            process.exitCode === null && process.signalCode === null
              ? new Promise<void>((resolve) =>
                  process.once('exit', () => {
                    resolve();
                  }),
                )
              : Promise.resolve(),
          ),
      );
      rmSync(dir, { recursive: true, force: true });
    }
  }, 40000);
});
