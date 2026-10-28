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
