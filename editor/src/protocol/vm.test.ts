// @vitest-environment node Reconciliation with live VM: the scheme is fresh, the TS
// types match it, messages through real `vish serve` give text equal to projection
// `vish fmt`.
import { spawn, execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdtempSync, readFileSync } from 'node:fs';
import { connect } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { FrameSplitter } from './codec';
import {
  decodeServer,
  encodeClient,
  projectionText,
  schema,
  type ServerMsg,
} from './messages';

const root = join(import.meta.dirname, '../../..');
const vish = join(root, 'out/vish');
const program = join(root, 'tests/editor_text.vish');

describe('живая VM', () => {
  it('schema.json совпадает с `vish schema`', () => {
    const live: unknown = JSON.parse(
      execFileSync(vish, ['schema'], { encoding: 'utf8' }),
    );
    const file: unknown = JSON.parse(
      readFileSync(join(import.meta.dirname, 'schema.json'), 'utf8'),
    );
    expect(file).toEqual(live);
  });

  it('поля типов TS — те же, что в схеме', () => {
    const fields = (t: string) => (schema.types[t] ?? []).map(([n]) => n);
    expect(fields('s_view')).toEqual([
      'view',
      'root',
      'revision',
      'nodes',
      'kind',
    ]);
    expect(fields('c_goto')).toEqual(['view', 'text']);
    expect(fields('c_layer')).toEqual(['view', 'key']);
    expect(fields('c_run')).toEqual(['stdin_text', 'args']);
    expect(fields('s_output')).toEqual([
      'text',
      'ok',
      'more',
      'state',
      'from',
      'now',
    ]);
    expect(fields('s_saved')).toEqual(['path']);
    expect(fields('c_trace')).toEqual(['stdin_text', 'args']);
    expect(fields('s_trace')).toEqual([
      'active',
      'step',
      'total',
      'node',
      'stack',
      'output',
      'error',
    ]);
    expect(fields('s_layers')).toEqual([
      'view',
      'layers',
      'selected',
      'overloads',
      'overload',
    ]);
    expect(fields('s_overload')).toEqual(['fn', 'head']);
    expect(fields('s_status')).toEqual([
      'kind',
      'text',
      'hint',
      'action',
      'label',
    ]);
    expect(fields('s_stops')).toEqual([
      'current',
      'stops',
      'prev',
      'next',
      'summary',
    ]);
    expect(fields('c_trace_step')).toEqual(['step']);
    expect(fields('s_node')).toEqual(['id', 'kind', 'pieces', 'note']);
    expect(fields('s_update')).toEqual(['view', 'revision', 'node', 'nodes']);
    expect(fields('s_complete')).toEqual([
      'view',
      'node',
      'piece',
      'offset',
      'selected',
      'items',
      'preview',
      'context',
    ]);
    expect(fields('s_item')).toEqual([
      'name',
      'kind',
      'detail',
      'result',
      'doc',
      'from',
      'to',
    ]);
    expect(fields('s_decor')).toEqual(['view', 'revision', 'marks']);
    expect(fields('s_mark')).toEqual(['node', 'kind', 'text', 'from', 'to']);
    expect(fields('s_cursor')).toEqual(['view', 'node', 'piece', 'offset']);
    expect(fields('c_key')).toEqual(['view', 'key', 'mods']);
    expect(fields('c_pointer')).toEqual(['view', 'node', 'piece', 'offset']);
    expect(fields('c_projection')).toEqual(['view', 'key']);
  });

  it('hello, представление и курсор через сокет', async () => {
    const sock = join(mkdtempSync(join(tmpdir(), 'vish-')), 's');
    const vm = spawn(vish, ['serve', program, sock], { stdio: 'ignore' });
    try {
      // up to 5 s: under load check.sh (asan, debug parallel) VM rises longer
      for (let i = 0; i < 250; ++i) {
        await new Promise((r) => setTimeout(r, 20));
        try {
          readFileSync(sock);
        } catch (e) {
          if ((e as NodeJS.ErrnoException).code !== 'ENOENT') break;
        }
      }
      const got: ServerMsg[] = [];
      const split = new FrameSplitter();
      const c = connect(sock);
      await new Promise<void>((ok, fail) => {
        c.on('connect', ok).on('error', fail);
      });
      c.on('data', (d: Buffer) => {
        split.push(new Uint8Array(d), (b) => {
          got.push(decodeServer(b));
        });
      });
      c.write(encodeClient({ type: 'c_hello', version: schema.version }));
      c.write(encodeClient({ type: 'c_open_view', kind: 'module' }));
      for (let i = 0; i < 500 && got.length < 4; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      const view = got.at(1);
      if (view?.type !== 's_view') throw new Error('нет представления');
      // first lines - comments of the program (§4.0): cursor - on the line main() {
      const fn = view.nodes.find((n) => n.kind === 'n_fn');
      if (!fn) throw new Error('нет функции');
      c.write(
        encodeClient({
          type: 'c_pointer',
          view: 1,
          node: fn.id,
          piece: 0,
          offset: 0,
        }),
      );
      c.write(encodeClient({ type: 'c_key', view: 1, key: 'End', mods: 0 }));
      for (let i = 0; i < 500 && got.length < 7; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      // after submission - check-session tags and status bangs
      expect(got.map((m) => m.type)).toEqual([
        's_hello',
        's_view',
        's_cursor',
        's_decor',
        's_status',
        's_cursor',
        's_cursor',
      ]);
      const decor = got.at(3);
      expect(
        decor?.type === 's_decor' && decor.marks.map((m) => m.kind),
      ).toContain('type');
      expect(projectionText(view)).toBe(
        execFileSync(vish, ['fmt', program], { encoding: 'utf8' }),
      );
      // input at the end of the first line: draft - text as typed (`y=2;`), leaving
      // the element (Escape) - canonical `y = 2;` in the block
      for (const key of ['y', '=', '2', ';']) {
        c.write(encodeClient({ type: 'c_key', view: 1, key, mods: 0 }));
      }
      const updates = () => got.filter((m) => m.type === 's_update').length;
      for (let i = 0; i < 500 && updates() < 4; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      const nodes = new Map(view.nodes.map((n) => [n.id, n]));
      const text = () => {
        for (const m of got) {
          if (m.type === 's_update')
            for (const n of m.nodes) nodes.set(n.id, n);
        }
        return projectionText({ ...view, nodes: [...nodes.values()] });
      };
      expect(text()).toContain('\nmain() {y=2;\n  s = "привет, мир";');
      // tags - by pause set (1 s), without requesting a window
      const decors = () => got.filter((m) => m.type === 's_decor').length;
      const before = decors();
      for (let i = 0; i < 200 && decors() === before; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      expect(decors()).toBeGreaterThan(before);
      c.write(encodeClient({ type: 'c_key', view: 1, key: 'Escape', mods: 0 }));
      for (let i = 0; i < 500 && updates() < 5; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      expect(text()).toContain(
        '\nmain() {\n  y = 2;\n  s = "привет, мир";\n  print(s);\n}\n',
      );
      // cursor - immediately after `;` : before translating a line in a piece of ";\n" block
      await new Promise((r) => setTimeout(r, 30)); // tail: cursor, addition
      const typed = got.filter((m) => m.type === 's_cursor').at(-1);
      const kids =
        typed?.type === 's_cursor' && typed.node !== null
          ? nodes.get(typed.node)
          : undefined;
      expect(kids?.kind).toBe('n_block');
      expect(typed).toMatchObject({ piece: 2, offset: 1 });
      c.end();
      const end = got.at(6);
      // end of the first line - before translating the line in a piece "{\n" of the block
      const block = view.nodes.find((n) => n.kind === 'n_block');
      expect(end).toMatchObject({ node: block?.id, piece: 0, offset: 1 });
    } finally {
      vm.kill();
    }
  });

  it('запуск и сохранение: F5 — текущее состояние, Ctrl+S — .vishdb, которую понимает vish run', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'vish-'));
    const prog = join(dir, 'p.vish');
    copyFileSync(join(root, 'tests/editor_overloads.vish'), prog);
    const sock = join(dir, 's');
    const vm = spawn(vish, ['serve', prog, sock], { stdio: 'ignore' });
    try {
      for (let i = 0; i < 50 && !existsSync(sock); ++i) {
        await new Promise((r) => setTimeout(r, 20));
      }
      const got: ServerMsg[] = [];
      const split = new FrameSplitter();
      const c = connect(sock);
      await new Promise<void>((ok, fail) => {
        c.on('connect', ok).on('error', fail);
      });
      c.on('data', (d: Buffer) => {
        split.push(new Uint8Array(d), (b) => {
          got.push(decodeServer(b));
        });
      });
      c.write(encodeClient({ type: 'c_hello', version: schema.version }));
      c.write(encodeClient({ type: 'c_run', stdin_text: '1.5 2', args: '' }));
      c.write(encodeClient({ type: 'c_save' }));
      const wait = async (t: string) => {
        for (let i = 0; i < 500 && !got.some((m) => m.type === t); ++i) {
          await new Promise((r) => setTimeout(r, 10));
        }
        return got.find((m) => m.type === t);
      };
      expect(await wait('s_output')).toMatchObject({
        text: '4\n2.25\n',
        ok: true,
      });
      expect(await wait('s_saved')).toMatchObject({ path: `${prog}db` });
      c.end();
      const rerun = execFileSync(vish, ['run', `${prog}db`], {
        input: '1.5 2',
        encoding: 'utf8',
      });
      expect(rerun).toBe('4\n2.25\n');
    } finally {
      vm.kill();
    }
  });

  it('версии: трёхстороннее слияние копий базы — CLI и редактор', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'vish-'));
    const prog = join(dir, 'p.vish');
    copyFileSync(join(root, 'tests/editor_overloads.vish'), prog);
    // Two copies diverge: “me” - area(i64) and main, “them” - area(f64) and main.
    const vmEdit = (script: string) =>
      execFileSync(vish, ['vm', prog], {
        input: script,
        encoding: 'utf8',
        cwd: dir,
      });
    vmEdit(
      'edit main\nmain() {\n  xs = read_floats(stdin);\n  print(area(xs.size));\n}\n.\nsave mine.vishdb\n',
    );
    vmEdit(
      'edit main\nmain() {\n  xs = read_floats(stdin);\n  print(area(xs[0]));\n}\n.\nsave theirs.vishdb\n',
    );
    // CLI: conflict in main - code 1, my version
    let cli = '';
    try {
      execFileSync(
        vish,
        [
          'merge',
          join(dir, 'mine.vishdb'),
          join(dir, 'theirs.vishdb'),
          join(dir, 'out.vishdb'),
        ],
        { encoding: 'utf8' },
      );
    } catch (e) {
      cli = (e as { stdout: string }).stdout;
    }
    expect(cli).toContain('конфликт: main()');
    // Editor: VM merger, conflict - both options; "take them"
    const sock = join(dir, 's');
    const vm = spawn(vish, ['serve', join(dir, 'mine.vishdb'), sock], {
      stdio: 'ignore',
    });
    try {
      for (let i = 0; i < 50 && !existsSync(sock); ++i) {
        await new Promise((r) => setTimeout(r, 20));
      }
      const got: ServerMsg[] = [];
      const split = new FrameSplitter();
      const c = connect(sock);
      await new Promise<void>((ok, fail) => {
        c.on('connect', ok).on('error', fail);
      });
      c.on('data', (d: Buffer) => {
        split.push(new Uint8Array(d), (b) => {
          got.push(decodeServer(b));
        });
      });
      const wait = async (pred: (m: ServerMsg) => boolean) => {
        for (let i = 0; i < 200 && !got.some(pred); ++i) {
          await new Promise((r) => setTimeout(r, 10));
        }
        return got.find(pred);
      };
      c.write(encodeClient({ type: 'c_hello', version: schema.version }));
      c.write(encodeClient({ type: 'c_open_view', kind: 'module' }));
      c.write(
        encodeClient({ type: 'c_merge', path: join(dir, 'theirs.vishdb') }),
      );
      const merge = await wait((m) => m.type === 's_merge');
      expect(merge).toMatchObject({ conflicts: [{ key: 'main()' }] });
      got.length = 0;
      c.write(encodeClient({ type: 'c_take', key: 'main()', theirs: true }));
      const view = await wait((m) => m.type === 's_view');
      if (view?.type !== 's_view') throw new Error('нет представления');
      expect(projectionText(view)).toContain('print(area(xs[0]));');
      expect(projectionText(view)).not.toContain('print(area(xs.size));');
      expect(await wait((m) => m.type === 's_merge')).toMatchObject({
        conflicts: [],
      });
      c.end();
    } finally {
      vm.kill();
    }
  });
});
