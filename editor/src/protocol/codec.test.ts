import { describe, expect, it } from 'vitest';
import { FrameSplitter, decode, frame, type Value } from './codec';
import { schema } from './messages';

describe('кодек по схеме', () => {
  it('сообщение проходит туда и обратно', () => {
    const msg = {
      type: 's_view',
      view: 1,
      root: 7,
      revision: 42,
      kind: 'module',
      projection: '',
      nodes: [
        {
          type: 's_node',
          id: 7,
          kind: 'n_fn',
          pieces: ['f(', 8, ') привет'],
          note: '',
        },
        { type: 's_node', id: 8, kind: 'n_param', pieces: ['x'], note: '' },
      ],
    };
    const bytes = frame(schema, schema.server, msg);
    const bodies: Uint8Array[] = [];
    new FrameSplitter().push(bytes, (b) => {
      bodies.push(b.slice());
    });
    expect(bodies).toHaveLength(1);
    const body = bodies.at(0);
    if (!body) throw new Error('нет кадра');
    expect(decode(schema, schema.server, body)).toEqual(msg);
  });

  it('узел «нет» — null, кадры режутся по любым границам', () => {
    const a = frame(schema, schema.server, {
      type: 's_cursor',
      view: 1,
      node: null,
      piece: 0,
      offset: 0,
    });
    const b = frame(schema, schema.server, { type: 's_error', text: 'ошибка' });
    const all = new Uint8Array([...a, ...b]);
    const split = new FrameSplitter();
    const got: Value[] = [];
    for (let i = 0; i < all.length; i += 3) {
      split.push(all.subarray(i, i + 3), (body) =>
        got.push(decode(schema, schema.server, body)),
      );
    }
    expect(got).toEqual([
      { type: 's_cursor', view: 1, node: null, piece: 0, offset: 0 },
      { type: 's_error', text: 'ошибка' },
    ]);
  });

  it('ответ репозитория: узел «нет», 64-битные номера и вложенные записи', () => {
    const msg = {
      type: 's_repository',
      request: 3,
      ok: true,
      revision: 12,
      path: '/tmp/a.vishrepo',
      branch: 'main',
      head: 2,
      dirty: false,
      message: 'ок',
      diff: '',
      entries: [
        {
          type: 'repository_entry',
          kind: 'module',
          id: 0,
          parent: 0,
          node: 7,
          name: 'geo',
          detail: '',
          head: 0,
        },
        {
          type: 'repository_entry',
          kind: 'branch',
          id: 0,
          parent: 0,
          node: null,
          name: 'main',
          detail: '',
          head: 2,
        },
      ],
    };
    const bytes = frame(schema, schema.server, msg);
    expect(decode(schema, schema.server, bytes.subarray(4))).toEqual(msg);
    const ask = {
      type: 'c_repository',
      request: 1,
      action: 'restore',
      key: '2',
      text: '',
      revision: 12,
    };
    expect(
      decode(
        schema,
        schema.client,
        frame(schema, schema.client, ask).subarray(4),
      ),
    ).toEqual(ask);
    const record = {
      type: 'c_record',
      kind: 'model_call',
      session: 's1',
      model: 'm',
      usage_in: 10,
      usage_out: 4,
      usage_cached: 2,
      usage_cache_write: 7,
      usage_thinking: 1,
      ms: 1234,
      error: '',
      request: 'q',
      response: 'a',
    };
    expect(
      decode(
        schema,
        schema.client,
        frame(schema, schema.client, record).subarray(4),
      ),
    ).toEqual(record);
  });

  it('лишние байты и обрыв — ошибки', () => {
    const f = frame(schema, schema.server, { type: 's_error', text: 'x' });
    const body = f.subarray(4);
    expect(() =>
      decode(schema, schema.server, new Uint8Array([...body, 0])),
    ).toThrow('лишние байты');
    expect(() => decode(schema, schema.server, body.subarray(0, 6))).toThrow(
      'неожиданный конец',
    );
  });
});
