import { describe, expect, it } from 'vitest';
import { schema } from '../../protocol/messages';
import { tagOf } from './index';
import { RecordNode } from './record_node';

describe('виды узлов', () => {
  it('на каждый вид из схемы VM зарегистрирован свой элемент', () => {
    const missing = schema.node_kinds.filter(
      (k) => !customElements.get(tagOf(k)),
    );
    expect(missing).toEqual([]);
  });

  // Common element (§4.5, step 11.4): sheet or composite - in pieces from VM, not in appearance.
  it('лист — текст в light DOM, составной — синтаксис в Shadow DOM, дети в slots', () => {
    const make = (kind: string) => {
      const el = document.createElement(tagOf(kind));
      if (!(el instanceof RecordNode)) throw new Error(kind);
      return el;
    };
    const leaf = make('n_ident');
    leaf.show(['x'], () => leaf);
    expect(leaf.composite).toBe(false);
    expect(leaf.textContent).toBe('x');

    const call = make('n_call');
    const arg = make('n_int');
    arg.show(['1'], () => arg);
    call.show(['each(', 7, ') s'], () => arg);
    expect(call.composite).toBe(true);
    expect(call.textContent).toBe('1'); // In light DOM only children
    expect(call.shadowRoot?.textContent).toBe('each() s');
    expect(arg.slot).toBe('c7');
  });
});
