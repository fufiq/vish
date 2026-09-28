import { describe, expect, it } from 'vitest';
import { schema } from '../../protocol/messages';
import { tagOf } from './index';

describe('виды узлов', () => {
  it('на каждый вид из схемы VM зарегистрирован свой элемент', () => {
    const missing = schema.node_kinds.filter(
      (k) => !customElements.get(tagOf(k)),
    );
    expect(missing).toEqual([]);
  });
});
