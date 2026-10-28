// The inspector of the topic “canvas” (frames “Tree Inspector” and “simple disclosure”): a tree is a line
// with the arrow у / (not on the way to focus - rolled up), a view icon, a view, text, a place in the parent,
// a recording parameter (→ c), a rule (who is disassembled) and “type = value”; above the tree - a legend.
// Block Disclosure: header "view · rule", parameters with substituted, "disclosure", "on the announcement",
// parameter chips with types. Data from VM (s_inspect).
import type { Inspect, InspectNode, NodeId } from '../../../protocol/messages';
import type { InspectActions } from '../../theme';
import { owner } from '../../inspector';
import { renderLines } from '../../view';

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
    e.stopPropagation();
    f();
  });
};

// Manually folded and manually deployed node (by default, everything is folded out of the way to focus)
const folded = new Set<NodeId>();
const opened = new Set<NodeId>();

// place in the parent "args[0] → c" → place and parameter
const split = (role: string): [string, string] => {
  const i = role.indexOf(' → ');
  return i < 0 ? [role, ''] : [role.slice(0, i), role.slice(i + 3)];
};

export function inspect(
  tree: HTMLElement,
  expand: HTMLElement,
  m: Inspect,
  act: InspectActions,
): void {
  const path = new Set<NodeId>();
  const stack: InspectNode[] = [];
  for (const n of m.nodes) {
    stack.length = n.depth;
    stack.push(n);
    if (n.id === m.focus) for (const x of stack) path.add(x.id);
  }
  const root = m.nodes.at(0);
  const head = make('div', 'ihead');
  head.append(
    make('span', 'title', 'Инспектор'),
    make('span', 'of', `дерево ${root?.text ?? ''}`),
  );
  const legend = make('div', 'legend');
  legend.append(
    make('span', 'place', 'место в родителе'),
    make('span', 'param', '→ параметр'),
    make('span', 'rule', 'кем разобран'),
    make('span', 'types', 'тип = значение'),
  );
  const rows: HTMLElement[] = [head, legend];
  let hideBelow = -1; // depth of the coiled ancestor (-1 - no)
  m.nodes.forEach((n, k) => {
    if (hideBelow >= 0 && n.depth > hideBelow) return;
    hideBelow = -1;
    const kids = (m.nodes[k + 1]?.depth ?? -1) > n.depth;
    const closed =
      kids && (folded.has(n.id) || (!path.has(n.id) && !opened.has(n.id)));
    if (closed) hideBelow = n.depth;
    const row = make('div', 'row');
    row.style.paddingLeft = `${String(6 + n.depth * 16)}px`;
    row.toggleAttribute('path', path.has(n.id));
    row.toggleAttribute('focus', n.id === m.focus);
    const arrow = make('span', 'arrow', kids ? (closed ? '▸' : '▾') : '');
    if (kids)
      press(arrow, () => {
        if (closed) {
          folded.delete(n.id);
          opened.add(n.id);
        } else {
          opened.delete(n.id);
          folded.add(n.id);
        }
        inspect(tree, expand, m, act);
      });
    const icon = make('i', 'icon');
    icon.dataset.kind = n.kind;
    const [place, param] = split(n.role);
    row.append(arrow, icon, make('span', 'kind', n.kind));
    if (n.text !== '') row.append(make('span', 'text', n.text));
    if (place !== '') row.append(make('span', 'place', place));
    if (param !== '') row.append(make('span', 'param', `→ ${param}`));
    if (n.rule !== '') {
      const rule = make('span', 'rule', n.rule);
      const at = n.rule_at;
      if (at !== null)
        press(rule, () => {
          act.open(at);
        });
      row.append(rule);
    }
    row.append(make('span', 'fill'), make('span', 'types', n.types));
    row.addEventListener('mouseenter', () => {
      act.mark(n.id);
    });
    row.addEventListener('mouseleave', () => {
      act.mark(null);
    });
    press(row, () => {
      act.place(n.id);
    });
    rows.push(row);
  });
  tree.replaceChildren(...rows);
  // full disclosure in the panel (mode "line") - header and lines
  const title = make('div', 'xhead');
  title.append(
    make('span', '', 'Раскрытие'),
    make('span', 'decl', m.expand.find((l) => l.text !== '')?.text ?? ''),
  );
  const lines = make('div', 'xlines');
  renderLines(lines, m.expand.slice(1), (node) => {
    act.place(node);
  });
  expand.replaceChildren(title, lines);
}

// one line: translations of lines and indentations - space
const flat = (s: string) => s.replace(/\s*\n\s*/g, ' ');

export function expansion(
  el: HTMLElement,
  m: Inspect,
  act: InspectActions,
): void {
  const own = owner(m);
  const focus = m.nodes.find((n) => n.id === own);
  const head = make('div', 'bhead');
  const icon = make('i', 'icon');
  icon.dataset.kind = focus?.kind ?? '';
  head.append(icon, make('span', 'kind', focus?.kind ?? ''));
  if (focus && focus.rule !== '')
    head.append(' · ', make('span', 'rule', focus.rule));
  const body = make('div', 'bbody');
  const decl = m.expand.at(0);
  for (const l of m.expand) {
    if (l.kind === 'параметр') {
      const eq = l.text.indexOf(' = ');
      const row = make('div', 'brow');
      row.append(
        make('span', 'pname', eq < 0 ? l.text : l.text.slice(0, eq)),
        make('span', 'eq', ' = '),
        make('span', 'code', eq < 0 ? '' : flat(l.text.slice(eq + 3))),
      );
      const target = l.target;
      if (target !== null)
        press(row, () => {
          act.place(target);
        });
      body.append(row);
    } else if (l.kind === 'раскрытие' || l.kind === 'итог') {
      const row = make('div', 'brow into');
      row.append(
        make('span', 'code', flat(l.text)),
        make('span', 'label', l.kind),
      );
      body.append(row);
    }
  }
  if (decl && decl.kind !== 'параметр') {
    const row = make('div', 'brow decl');
    row.append(
      make('span', 'label', 'по объявлению'),
      make('span', 'code', flat(decl.text)),
    );
    body.append(row);
  }
  // parameter chips with types - made of wood (place "→name")
  const chips = make('div', 'chips');
  const at = m.nodes.findIndex((n) => n.id === own);
  const depth = m.nodes.at(at)?.depth ?? 0;
  const kids: InspectNode[] = [];
  for (let k = at + 1; k > 0 && k < m.nodes.length; ++k) {
    const n = m.nodes.at(k);
    if (!n || n.depth <= depth) break;
    if (n.depth === depth + 1) kids.push(n);
  }
  for (const n of kids) {
    const [, param] = split(n.role);
    if (param === '') continue;
    const type = n.types.split(' = ')[0] ?? '';
    const chip = make(
      'span',
      'chip',
      type === '' ? param : `${param} : ${type}`,
    );
    chips.append(chip);
  }
  if (chips.childElementCount > 0) body.append(chips);
  el.replaceChildren(head, body);
}
