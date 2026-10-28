// Merging the theme “canvas” (frame “Laptop · Version merger”): the card on the right is a cap with a merged
// copy and Ctrl+M, chips “taken by yourself” and “conflict”, the conflict is a cap “key · what happened” and
// three columns base / mine / them with a difference and buttons; at the bottom - an explanation. Under the
// code of the module - taken by itself lines with a cause. Data from VM (s_merge).
//
import type { Conflict, MergeResult } from '../../../protocol/messages';
import type { Host } from '../../theme';

const make = <K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className = '',
  text = '',
): HTMLElementTagNameMap[K] => {
  const el = document.createElement(tag);
  if (className !== '') el.className = className;
  if (text !== '') el.textContent = text;
  return el;
};

const press = (el: HTMLElement, f: () => void) => {
  el.addEventListener('mousedown', (e) => {
    e.preventDefault();
    f();
  });
};

// text of the variant: [from, to) code points - modified
function marked(text: string, from: number, to: number): HTMLElement {
  const pre = make('pre', 'code');
  // VM eslint-disable-next-line @typescript-eslint/no-misused-spread
  //
  const cps = [...text];
  if (to <= from) pre.textContent = text;
  else
    pre.append(
      cps.slice(0, from).join(''),
      make('span', 'diff', cps.slice(from, to).join('')),
      cps.slice(to).join(''),
    );
  return pre;
}

function conflict(c: Conflict, host: Host): HTMLElement {
  const box = make('div', 'conflict');
  const head = make('div', 'chead');
  head.append(
    make('i', 'icon fn'),
    make('span', 'key', c.key),
    make('span', 'why', c.why),
  );
  const col = (
    side: string,
    label: string,
    note: string,
    text: string,
    from: number,
    to: number,
    take?: { label: string; theirs: boolean },
  ) => {
    const el = make('div', 'side');
    el.dataset.side = side;
    const top = make('div', 'stop');
    top.append(make('b', '', label), make('span', 'note', note));
    el.append(top, marked(text, from, to));
    if (take) {
      const b = make('button', '', take.label);
      press(b, () => {
        host.send({ type: 'c_take', key: c.key, theirs: take.theirs });
      });
      el.append(b);
    }
    return el;
  };
  const cols = make('div', 'cols');
  cols.append(
    col('base', 'база', 'общее начало', c.base, 0, 0),
    col('mine', 'мой', c.mine_note, c.mine, c.mine_from, c.mine_to, {
      label: 'Оставить мой',
      theirs: false,
    }),
    col('theirs', 'их', c.theirs_note, c.theirs, c.theirs_from, c.theirs_to, {
      label: 'Взять их',
      theirs: true,
    }),
  );
  box.append(head, cols);
  return box;
}

export function merge(el: HTMLElement, m: MergeResult, host: Host): void {
  el.hidden = m.conflicts.length === 0;
  const head = make('div', 'mhead');
  head.append(
    make('i', 'icon merge'),
    make('span', 'title', 'Слияние'),
    make('span', 'path', m.path),
    make('kbd', '', 'Ctrl+M'),
  );
  const chips = make('div', 'chips');
  for (const k of m.taken) {
    const chip = make('span', 'chip taken', 'взято само: ');
    chip.prepend(make('i', 'icon ok'));
    chip.append(make('code', '', k));
    chips.append(chip);
  }
  for (const c of m.conflicts) {
    const chip = make('span', 'chip clash', 'конфликт: ');
    chip.prepend(make('i', 'icon warn'));
    chip.append(make('code', '', c.key));
    chips.append(chip);
  }
  chips.append(
    make('span', 'fill'),
    make('span', 'base', 'общее начало журналов — база'),
  );
  const list = make('div', 'list');
  list.append(...m.conflicts.map((c) => conflict(c, host)));
  el.replaceChildren(
    head,
    chips,
    list,
    make(
      'div',
      'note',
      'Изменённое только у них берётся само, с раскладкой (внутренний, пустые строки, комментарий). Изменённое у обоих — решаешь руками; автослияния нет.',
    ),
  );
}

export function mergeNotes(el: HTMLElement, m: MergeResult): void {
  el.hidden = m.taken.length === 0;
  el.replaceChildren(
    ...m.taken.map((k, i) => {
      const row = make('div', 'taken');
      row.append(
        make('i', 'icon ok'),
        make('code', '', k),
        ` ${m.taken_why[i] ?? ''}`,
      );
      return row;
    }),
  );
}
