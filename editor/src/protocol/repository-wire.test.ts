// @vitest-environment node
import { spawn } from 'node:child_process';
import {
  copyFileSync,
  existsSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { connect } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import { FrameSplitter } from './codec';
import {
  decodeServer,
  encodeClient,
  projectionText,
  schema,
  type ClientMsg,
  type ServerMsg,
} from './messages';
const vish = join(import.meta.dirname, '../../../out/vish');
async function service(program: string, socketPath: string) {
  const child = spawn(vish, ['serve', program, socketPath]);
  let stderr = '';
  child.stderr.on('data', (x: Buffer) => {
    stderr += x.toString();
  });
  for (
    let i = 0;
    i < 250 && !existsSync(socketPath) && child.exitCode === null;
    ++i
  )
    await new Promise((resolve) => setTimeout(resolve, 20));
  if (!existsSync(socketPath)) {
    child.kill();
    throw new Error(`VM не запустилась: ${stderr}`);
  }
  const socket = connect(socketPath);
  await new Promise<void>((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });
  const waiters = new Set<{
    accept: (m: ServerMsg) => void;
    reject: (e: Error) => void;
  }>();
  const split = new FrameSplitter();
  socket.on('data', (data: Buffer) => {
    split.push(new Uint8Array(data), (body) => {
      for (const w of [...waiters]) w.accept(decodeServer(body));
    });
  });
  socket.on('error', (e) => {
    for (const w of [...waiters]) w.reject(e);
  });
  socket.on('close', () => {
    for (const w of [...waiters])
      w.reject(new Error(`VM отключилась: ${stderr}`));
  });
  function wait<K extends ServerMsg['type']>(
    type: K,
    predicate: (m: Extract<ServerMsg, { type: K }>) => boolean = () => true,
  ) {
    return new Promise<Extract<ServerMsg, { type: K }>>((resolve, reject) => {
      const finish = () => {
        clearTimeout(timer);
        waiters.delete(w);
      };
      const w = {
        accept(m: ServerMsg) {
          if (m.type === 's_error') {
            finish();
            reject(new Error(m.text));
          } else if (m.type === type) {
            const selected = m as Extract<ServerMsg, { type: K }>;
            if (predicate(selected)) {
              finish();
              resolve(selected);
            }
          }
        },
        reject(e: Error) {
          finish();
          reject(e);
        },
      };
      const timer = setTimeout(() => {
        finish();
        reject(new Error(`нет ${type}: ${stderr}`));
      }, 10000);
      waiters.add(w);
    });
  }
  const send = (m: ClientMsg) => socket.write(encodeClient(m));
  const hello = wait('s_hello');
  send({ type: 'c_hello', version: schema.version });
  await hello;
  let request = 0,
    revision = 0;
  async function repo(action: string, key = '', text = '', expected?: number) {
    const id = ++request;
    const reply = wait('s_repository', (m) => m.request === id);
    send({
      type: 'c_repository',
      request: id,
      action,
      key,
      text,
      revision: action === 'info' ? 0 : (expected ?? revision),
    });
    const result = await reply;
    revision = result.revision;
    return result;
  }
  async function stop() {
    socket.destroy();
    if (child.exitCode === null && child.signalCode === null) {
      const ended = new Promise<void>((resolve) =>
        child.once('exit', () => {
          resolve();
        }),
      );
      child.kill();
      await ended;
    }
  }
  return { wait, send, repo, stop };
}
it('редактирует модуль, отвергает старую ревизию и восстанавливает черновик после автосохранения', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'vish-repository-wire-'));
  const seed = join(dir, 'seed.vish'),
    path = join(dir, 'program.vishrepo');
  writeFileSync(seed, 'main() => print(1);\n');
  let vm: Awaited<ReturnType<typeof service>> | undefined;
  try {
    vm = await service(seed, join(dir, 'first.sock'));
    const first = vm.wait('s_view');
    vm.send({ type: 'c_open_view', kind: 'module' });
    await first;
    expect((await vm.repo('info')).path).toBe('');
    const initialized = await vm.repo('init', path);
    expect(initialized.ok).toBe(true);
    const created = await vm.repo('module', 'math', 'twice(i64 x) => x + x;');
    expect(created.ok).toBe(true);
    expect(created.revision).toBeGreaterThan(initialized.revision);
    const node = created.entries.find(
      (x) => x.kind === 'module' && x.name === 'math',
    )?.node;
    expect(node).toBeTypeOf('number');
    const stale = await vm.repo(
      'snapshot',
      'Старый запрос',
      '',
      initialized.revision,
    );
    expect(stale.ok).toBe(false);
    expect(stale.message).toContain('ревизия изменилась');
    expect(stale.entries.some((x) => x.name === 'Старый запрос')).toBe(false);
    const saved = await vm.repo('snapshot', 'Два модуля');
    expect(saved.ok).toBe(true);
    const forked = await vm.repo('fork', 'dev');
    expect(forked.revision).toBeGreaterThan(saved.revision);
    const opening = vm.wait('s_view', (m) => m.root === node);
    expect((await vm.repo('open', String(node))).ok).toBe(true);
    const view = await opening;
    const autosaved = vm.wait(
      's_repository',
      (m) => m.request === 0 && m.dirty,
    );
    vm.send({ type: 'c_key', view: view.view, key: 'a', mods: 2 });
    vm.send({
      type: 'c_input',
      view: view.view,
      text: 'module math;\ntwice(i64 x) => x + x + 1;\nbroken(i64 x) => x +',
    });
    await autosaved;
    await vm.stop();
    vm = undefined;
    rmSync(seed);
    vm = await service(path, join(dir, 'second.sock'));
    const restored = await vm.repo('info');
    expect(restored.ok).toBe(true);
    expect(
      restored.entries.find((x) => x.kind === 'module' && x.name === 'math')
        ?.node,
    ).toBe(node);
    const initialView = vm.wait('s_view');
    vm.send({ type: 'c_open_view', kind: 'module' });
    await initialView;
    const recovering = vm.wait('s_view', (m) => m.root === node);
    expect((await vm.repo('open', String(node))).ok).toBe(true);
    const shown = projectionText(await recovering);
    expect(shown).toContain('twice');
    expect(shown).toContain('+ 1');
    expect(shown).toContain('broken');
    expect(shown).toContain('x +');
  } catch (error) {
    if (existsSync(path))
      copyFileSync(
        path,
        join(tmpdir(), 'vish-repository-wire-failed.vishrepo'),
      );
    throw error;
  } finally {
    await vm?.stop();
    rmSync(dir, { recursive: true, force: true });
  }
}, 30000);
