// The palette of the transition theme "canvas" (frame "Laptop · transition and add to the place"): on
// the left points - "add" from the top with a turquoise plus, a line, overload with an icon, a painted
// signature and a reason from VM; on the right "will be created" and an explanation from VM; at the
// bottom of the key. “I’m trying to find overload for foo(int), but I can write foo(int) and go.”
import type { Complete } from '../../../protocol/messages';

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

const kbd = (text: string) => make('kbd', '', text);

// Signature "squares(explicit f64 n)" in parts: name, brackets, types and names of typesOnly
// parameters - in brackets only types
function signature(sig: string, typesOnly = false): HTMLElement {
  const out = make('span', 'sig');
  const open = sig.indexOf('(');
  if (open < 0) {
    out.append(make('span', 'fn', sig));
    return out;
  }
  out.append(
    make('span', 'fn', sig.slice(0, open)),
    make('span', 'punct', '('),
  );
  const inside = sig.slice(open + 1, sig.lastIndexOf(')'));
  inside.split(', ').forEach((p, i) => {
    if (i > 0) out.append(make('span', 'punct', ', '));
    const words = p.split(' ');
    const name = words.pop() ?? '';
    if (typesOnly) out.append(make('span', 'type', p));
    else {
      if (words.length > 0)
        out.append(make('span', 'type', `${words.join(' ')} `));
      out.append(make('span', 'param', name));
    }
  });
  out.append(make('span', 'punct', ')'));
  return out;
}

export function palette(el: HTMLElement, m: Complete | null): void {
  el.replaceChildren();
  if (!m || m.items.length === 0) return;
  const list = make('div', 'list');
  for (const [i, it] of m.items.entries()) {
    const add = it.kind === 'добавить';
    const row = make('div', add ? 'row add' : 'row');
    row.toggleAttribute('selected', i === m.selected);
    row.append(make('i', add ? 'icon plus' : 'icon fn'));
    const text = make('span', 'text');
    if (add) {
      text.append(
        make('span', 'verb', 'добавить '),
        signature(it.name.replace(/^добавить /, ''), true),
      );
    } else text.append(signature(it.name));
    row.append(text, make('span', 'why', it.detail));
    list.append(row);
    if (add && m.items.at(i + 1)?.kind !== 'добавить')
      list.append(make('div', 'sep'));
  }
  const body = make('div', 'body');
  body.append(list);
  if (m.preview !== '') {
    const side = make('div', 'side');
    const head = make('div', 'head');
    head.append(make('i', 'icon eye'), 'будет создано');
    side.append(
      head,
      make('pre', 'code', m.preview.replace(/\n$/, '')),
      make('div', 'note', m.context),
    );
    body.append(side);
  }
  const foot = make('div', 'foot');
  foot.append(
    make('span', '', 'с места вызова — тот же список:'),
    kbd('F12'),
    make('span', 'fill'),
    kbd('↑↓'),
    kbd('↵'),
    make(
      'span',
      '',
      m.items[m.selected]?.kind === 'добавить' ? 'добавить' : 'перейти',
    ),
    kbd('Esc'),
  );
  el.append(body, foot);
}
