import { describe, expect, it } from 'vitest';
import { projectionText, type View } from '../protocol/messages';
import { pieceOfText } from './nodes/ast_node';
import { defineNodes } from './nodes/index';
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
  kind: 'module',
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

defineNodes();
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
    const t = v.byId.get(12)?.textsOf(1)[0];
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

  it('список дополнения: пункты и выбранный — как прислала VM', () => {
    // In jsdom there is no layout - rectangle of space zero
    Range.prototype.getBoundingClientRect = () => new DOMRect();
    Element.prototype.scrollIntoView = () => undefined;
    const host = document.createElement('div');
    const v = new ViewElement(host, view);
    const item = (name: string) => ({
      type: 's_item' as const,
      name,
      kind: 'функция',
      detail: '(x)',
      result: '',
      doc: '',
      from: 0,
      to: 0,
    });
    v.showCompletion({
      type: 's_complete',
      view: 1,
      node: 13,
      piece: 0,
      offset: 0,
      selected: 1,
      items: [item('print'), item('port')],
      preview: '',
      context: '',
    });
    expect(v.popup.hidden).toBe(false);
    expect(
      [...v.popup.children].map((li) => li.hasAttribute('selected')),
    ).toEqual([false, true]);
    expect(v.popup.children.item(1)?.textContent).toBe('port(x) функция');
    v.showCompletion({
      type: 's_complete',
      view: 1,
      node: 13,
      piece: 0,
      offset: 0,
      selected: 0,
      items: [],
      preview: '',
      context: '',
    });
    expect(v.popup.hidden).toBe(true);
  });

  it('метки: значок типа в shadow узла, ошибка — атрибут и подсказка', () => {
    const host = document.createElement('div');
    const v = new ViewElement(host, view);
    const mark = (node: number, kind: string, text: string) => ({
      type: 's_mark' as const,
      node,
      kind,
      text,
      from: 0,
      to: 0,
    });
    const first = v.showMarks({
      type: 's_decor',
      view: 1,
      revision: 3,
      marks: [
        mark(13, 'type', 'str'),
        mark(14, 'error', 'не так\n  в main'),
        mark(12, 'dead', 'x'),
        mark(13, 'value', '1.5'),
      ],
    });
    expect(first).toBe('не так\n  в main');
    const s = v.byId.get(13);
    expect(s?.textContent).toBe('s'); // leaflet
    expect(s?.shadowRoot?.querySelector('.badge')?.textContent).toBe(
      'str = 1.5',
    );
    expect(v.byId.get(12)?.hasAttribute('dead')).toBe(true);
    expect(v.byId.get(14)?.hasAttribute('error')).toBe(true);
    expect(v.byId.get(14)?.title).toBe('не так\n  в main');
    v.showMarks({ type: 's_decor', view: 1, revision: 4, marks: [] });
    expect(s?.shadowRoot?.querySelector('.badge')).toBeNull();
    expect(v.byId.get(14)?.hasAttribute('error')).toBe(false);
    expect(v.byId.get(12)?.hasAttribute('dead')).toBe(false);
  });

  it('метка ошибки внутри сырого блока — диапазон его текста', () => {
    const host = document.createElement('div');
    const raw: View = {
      type: 's_view',
      view: 1,
      root: 20,
      revision: 1,
      kind: 'module',
      nodes: [
        {
          type: 's_node',
          id: 20,
          kind: 'n_raw',
          pieces: ['{#ffg00aa}'],
          note: '',
        },
      ],
    };
    const v = new ViewElement(host, raw);
    v.showMarks({
      type: 's_decor',
      view: 1,
      revision: 2,
      marks: [
        {
          type: 's_mark',
          node: 20,
          kind: 'error',
          text: 'нет',
          from: 4,
          to: 9,
        },
      ],
    });
    expect(v.errorRanges.map((r) => r.toString())).toEqual(['g00aa']);
    expect(v.byId.get(20)?.hasAttribute('error')).toBe(true);
    v.showMarks({ type: 's_decor', view: 1, revision: 3, marks: [] });
    expect(v.errorRanges).toEqual([]);
  });

  it('смещения: кодовые точки ↔ UTF-16', () => {
    expect(codePoints('при🙂')).toBe(4);
    expect(utf16Offset('при🙂x', 4)).toBe(5);
    expect(utf16Offset('аб', 5)).toBe(2);
  });
});
