// Extensions panel canvas theme - ALL visual here and in extensions.css (change them, nothing else). What shows:
// the editor expansion points connected to the program are `[[editor]]` functions of the program itself and its
// plugins (Vish modules connected to `use`). The list sends VM (`s_extensions`: module, name, signature, view,
// whether covered); the client only draws a plugin - a module in the language: No frame in the design - a view
// modeled on the cards "canvas"; *my*.
//
import type { Extensions } from '../../../protocol/messages';

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

// Type of chip signature
const kinds: Record<string, string> = {
  decision: 'решение',
  visual: 'схема',
  other: 'своё',
};

export function extensions(el: HTMLElement, m: Extensions): void {
  // groups by module in search order: program, then plugins
  const groups = new Map<string, Extensions['points']>();
  for (const p of m.points) {
    const g = groups.get(p.module);
    if (g) g.push(p);
    else groups.set(p.module, [p]);
  }
  const head = make('div', 'head');
  head.append(
    make('span', 'title', 'Расширения'),
    make('span', 'count', String(m.points.length)),
  );
  const body = make('div', 'groups');
  if (m.points.length === 0)
    body.append(
      make(
        'div',
        'empty',
        'Нет точек расширения: добавьте функцию с [[editor]] в программу или подключите плагин (use).',
      ),
    );
  for (const [module, points] of groups) {
    const group = make('section', 'group');
    group.append(
      make('div', 'module', module === '' ? 'программа' : module),
      ...points.map((p) => {
        const row = make('div', p.shadowed ? 'point shadowed' : 'point');
        const chip = make('span', `chip ${p.kind}`, kinds[p.kind] ?? p.kind);
        row.append(chip, make('code', 'sig', p.sig));
        if (p.shadowed) row.append(make('span', 'note', 'перекрыта'));
        return row;
      }),
    );
    body.append(group);
  }
  el.replaceChildren(head, body);
}
