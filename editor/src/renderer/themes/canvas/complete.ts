// The list of additions to the topic “canvas” (frame “Laptop · draft”): the icon of the view, the name with
// the matching part, parameters, the type of result; the selected one has and a line of documentation; the
// basement is the recipient and from where, the keys ↑↓ Esc. Data is from VM (s_complete); only Enter
// accepts.
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

// Item view → icon role (like entity in code)
const roles: Record<string, string> = {
  функция: 'prelude',
  переменная: 'var',
  параметр: 'param',
  тип: 'type',
  модуль: 'module',
};

export function complete(el: HTMLElement, m: Complete): void {
  el.dataset.place = 'above';
  const rows: HTMLElement[] = [];
  for (const [i, it] of m.items.entries()) {
    const li = make('li', 'item');
    const selected = i === m.selected;
    li.toggleAttribute('selected', selected);
    const icon = make('i', 'icon');
    icon.dataset.role = roles[it.kind] ?? 'var';
    // eslint-disable-next-line @typescript-eslint/no-misused-spread
    const cps = [...it.name];
    const name = make('span', 'name');
    if (it.to > it.from)
      name.append(
        cps.slice(0, it.from).join(''),
        make('b', '', cps.slice(it.from, it.to).join('')),
        cps.slice(it.to).join(''),
      );
    else name.textContent = it.name;
    const sig = make('span', 'sig', it.kind === 'функция' ? it.detail : '');
    li.append(icon, name, sig, make('span', 'fill'));
    if (it.kind !== 'функция' && it.detail !== '')
      li.append(make('span', 'type', it.detail));
    else if (it.result !== '' && it.result !== 'unit')
      li.append(make('span', 'type', it.result));
    if (selected) li.append(make('kbd', '', '↵'));
    rows.push(li);
    if (selected && it.doc !== '') rows.push(make('li', 'doc', it.doc));
  }
  const foot = make('li', 'foot');
  foot.append(
    make('span', 'context', m.context),
    make('kbd', '', '↑↓'),
    make('kbd', '', 'Esc'),
  );
  el.replaceChildren(...rows, foot);
}
