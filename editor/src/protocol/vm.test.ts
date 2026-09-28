// @vitest-environment node Reconciliation with live VM: the scheme is fresh, the TS
// types match it, messages through real `tz serve` give text equal to projection `tz
// fmt`.
import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, mkdtempSync } from 'node:fs';
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
const tz = join(root, 'out/tz');
const program = join(root, 'tests/editor_text.tz');

describe('живая VM', () => {
  it('schema.json совпадает с `tz schema`', () => {
    const live: unknown = JSON.parse(
      execFileSync(tz, ['schema'], { encoding: 'utf8' }),
    );
    const file: unknown = JSON.parse(
      readFileSync(join(import.meta.dirname, 'schema.json'), 'utf8'),
    );
    expect(file).toEqual(live);
  });

  it('поля типов TS — те же, что в схеме', () => {
    const fields = (t: string) => (schema.types[t] ?? []).map(([n]) => n);
    expect(fields('s_view')).toEqual(['view', 'root', 'revision', 'nodes']);
    expect(fields('s_node')).toEqual(['id', 'kind', 'pieces']);
    expect(fields('s_cursor')).toEqual(['view', 'node', 'piece', 'offset']);
    expect(fields('c_key')).toEqual(['view', 'key', 'mods']);
    expect(fields('c_pointer')).toEqual(['view', 'node', 'piece', 'offset']);
  });

  it('hello, представление и курсор через сокет', async () => {
    const sock = join(mkdtempSync(join(tmpdir(), 'tz-')), 's');
    const vm = spawn(tz, ['serve', program, sock], { stdio: 'ignore' });
    try {
      for (let i = 0; i < 50; ++i) {
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
      c.write(encodeClient({ type: 'c_key', view: 1, key: 'End', mods: 0 }));
      for (let i = 0; i < 100 && got.length < 4; ++i) {
        await new Promise((r) => setTimeout(r, 10));
      }
      c.end();
      expect(got.map((m) => m.type)).toEqual([
        's_hello',
        's_view',
        's_cursor',
        's_cursor',
      ]);
      const view = got.at(1);
      if (view?.type !== 's_view') throw new Error('нет представления');
      expect(projectionText(view)).toBe(
        execFileSync(tz, ['fmt', program], { encoding: 'utf8' }),
      );
      const end = got.at(3);
      // end of the first line - before translating the line in a piece "{\n" of the block
      const block = view.nodes.find((n) => n.kind === 'n_block');
      expect(end).toMatchObject({ node: block?.id, piece: 0, offset: 1 });
    } finally {
      vm.kill();
    }
  });
});
