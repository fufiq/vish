import { describe, expect, it } from 'vitest';
import { projectionText, type View } from '../protocol/messages';
import { pieceOfText } from './nodes/ast_node';
import { codePoints, utf16Offset, ViewElement } from './view';

// Text that a person sees: shadow scenery + children by their slots.
function composed(el: Element): string {
  const root = el.shadowRoot;
  if (!root) return el.textContent;
  let out = '';
  for (const part of root.childNodes) {
    if (part instanceof HTMLSlotElement) {
      for (const kid of part.assignedElements()) out += composed(kid);
    } else out += part.textContent ?? '';
  }
  return out;
}

const view: View = {
  type: 's_view',
  view: 1,
  root: 10,
  revision: 1,
  nodes: [
    { type: 's_node', id: 10, kind: 'n_fn', pieces: ['main() ', 11], note: '' },
    {
      type: 's_node',
      id: 11,
      kind: 'n_block',
      pieces: ['{\n  ', 12, ';\n}'],
      note: '',
    },
    { type: 's_node', id: 12, kind: 'n_op', pieces: [13, ' = ', 14], note: '' },
    { type: 's_node', id: 13, kind: 'n_ident', pieces: ['s'], note: '' },
    {
      type: 's_node',
      id: 14,
      kind: 'n_str',
      pieces: ['"привет 🙂"'],
      note: '',
    },
  ],
};

describe('представление', () => {
  it('DOM — проекция: собранный текст равен тексту кусков', () => {
    const host = document.createElement('div');
    const v = new ViewElement(host, view);
    expect(v.root && composed(v.root)).toBe(projectionText(view));
    expect(projectionText(view)).toBe('main() {\n  s = "привет 🙂";\n}');
  });

  it('листья — в light DOM, синтаксис составных — в shadow', () => {
    const host = document.createElement('div');
    const v = new ViewElement(host, view);
    expect(v.byId.get(14)?.shadowRoot).toBeNull();
    expect(v.byId.get(14)?.textContent).toBe('"привет 🙂"');
    expect(v.byId.get(11)?.shadowRoot?.textContent).toBe('{\n  ;\n}');
    const t = v.byId.get(12)?.textOf(1);
    expect(t && pieceOfText.get(t)).toEqual({ node: 12, piece: 1 });
  });

  it('update пересобирает поддерево на месте, остальное не трогает', () => {
    const host = document.createElement('div');
    const v = new ViewElement(host, view);
    const fn = v.byId.get(10);
    v.update({
      type: 's_update',
      view: 1,
      revision: 2,
      node: 11,
      nodes: [
        {
          type: 's_node',
          id: 11,
          kind: 'n_block',
          pieces: ['{\n  ', 20, '\n}'],
          note: '',
        },
        {
          type: 's_node',
          id: 20,
          kind: 'n_hole',
          pieces: ['s ='],
          note: 'ожидалось выражение',
        },
      ],
    });
    expect(v.byId.get(10)).toBe(fn);
    expect(v.root && composed(v.root)).toBe('main() {\n  s =\n}');
    const hole = v.byId.get(20);
    expect(hole?.title).toBe('ожидалось выражение');
    expect(hole?.hasAttribute('noted')).toBe(true);
    expect(v.byId.get(11)?.slot).toBe('c11');
  });

  it('смещения: кодовые точки ↔ UTF-16', () => {
    expect(codePoints('при🙂')).toBe(4);
    expect(utf16Offset('при🙂x', 4)).toBe(5);
    expect(utf16Offset('аб', 5)).toBe(2);
  });
});
