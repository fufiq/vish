// @vitest-environment node
import { execFileSync, spawn } from 'node:child_process';
import {
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
it('принятие предложения ИИ в репозитории: снимок и цепочка причин в журнале объектов', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'vish-runlog-wire-'));
  const seed = join(dir, 'seed.vish'),
    path = join(dir, 'program.vishrepo');
  writeFileSync(
    seed,
    'f(x) => x + 1;\nmain() => print(f(2));\n',
  );
  execFileSync(vish, ['repo', 'init', seed, path]);
  let vm: Awaited<ReturnType<typeof service>> | undefined;
  try {
    const live = await service(path, join(dir, 'a.sock'));
    vm = live;
    const view = vm.wait('s_view');
    vm.send({ type: 'c_open_view', kind: 'module' });
    await view;
    vm.send({ type: 'c_name', name: 'ИИ' });
    let request = 100;
    async function ask(
      action: string,
      workspace: number,
      key = '',
      text = '',
      revision = 0,
      args = '',
      input = '',
      expected = '',
    ) {
      const id = ++request;
      const reply = live.wait('s_agent', (m) => m.request === id);
      live.send({
        type: 'c_workspace',
        request: id,
        workspace,
        action,
        revision,
        key,
        text,
        args,
        input,
        expected,
      });
      return reply;
    }
    const created = await ask('create', 0);
    expect(created.ok).toBe(true);
    const w = created.workspace;
    expect((await ask('edit', w, 'f(_)', 'f(x) => x + 3;')).ok).toBe(true);
    expect((await ask('check', w)).ok).toBe(true);
    const offered = await ask('offer', w, '', 'исправление');
    expect(offered.ok).toBe(true);
    const decided = vm.wait('s_status', (m) => m.kind === 'done');
    vm.send({ type: 'c_decide', id: offered.proposal, accept: true });
    await decided;
    const chain = await ask('provenance', 0, '', '', 6);
    expect(chain.text).toContain('decision');
    expect(chain.text).toContain('snapshot=');
    const info = await vm.repo('info');
    expect(info.entries.some((x) => x.name.includes('ИИ: принято'))).toBe(true);
    expect(existsSync(`${path}.runlog`)).toBe(true);
  } finally {
    await vm?.stop();
    rmSync(dir, { recursive: true, force: true });
  }
}, 60000);
