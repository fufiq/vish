// Card entity theme "canvas" (frame "Laptop · cursor on stream"): diagram of the place on top, below -
// signature, name and documentation with entity chips. A circuit is a component in the form of a line of
// “circuit” (visuals); there is no form - a line of the scheme list.
//
import type { HintLine, NodeId } from '../../../protocol/messages';
import type { Host } from '../../theme';
import { renderLines } from '../../view';

type Open = (node: NodeId) => void;

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

// role from VM (var 2, prelude) → attributes: the theme paints the chip as entity in code
const role = (el: HTMLElement, r: string): HTMLElement => {
  const [kind = '', entity = ''] = r.split(' ');
  el.dataset.role = kind;
  if (entity !== '') el.dataset.entity = entity;
  return el;
};

const link = (el: HTMLElement, target: NodeId | null, open: Open) => {
  if (target === null) return el;
  el.classList.add('link');
  el.addEventListener('mousedown', (e) => {
    e.preventDefault();
    e.stopPropagation();
    open(target);
  });
  return el;
};

// Flow Scheme: Writers (function and what writes) → container cells with running values → readers.
function streamVisual(lines: readonly HintLine[], open: Open): HTMLElement {
  const get = (k: string) => lines.find((l) => l.kind === `схема ${k}`);
  const all = (k: string) => lines.filter((l) => l.kind === `схема ${k}`);
  const box = make('div', 'visual stream');
  const writers = make('div', 'ends');
  const values = all('значение');
  for (const [i, w] of all('пишет').entries()) {
    const el = make('span', 'writer');
    el.append(make('i', 'icon'), make('b', '', w.text));
    writers.append(link(role(el, 'fn'), w.target, open));
    const v = values.at(i);
    if (v) el.title = `пишет ${v.text}`;
  }
  const s = get('поток');
  const [name = '', r = ''] = (s?.text ?? '').split('|');
  const pipe = make('div', 'pipe');
  const head = make('div', 'head');
  head.append(
    link(role(make('b', 'name', name), r), s?.target ?? null, open),
    ` : ${get('тип')?.text ?? 'stream'}`,
  );
  const cap = Math.min(
    8,
    Math.max(1, Number(get('ёмкость')?.text ?? '16') || 16),
  );
  const slots = make('div', 'slots');
  slots.style.setProperty('--cap', String(cap));
  for (let i = 0; i < cap; ++i) slots.append(make('span', 'slot'));
  const value = values[0]?.text ?? '';
  // values run through the cells: each - its own delay
  for (let i = 0; i < Math.min(cap, 4); ++i) {
    const chip = make('span', 'value', value);
    chip.style.setProperty('--i', String(i));
    slots.append(chip);
  }
  pipe.append(head, slots);
  const readers = make('div', 'ends');
  const into = all('в');
  for (const [i, rd] of all('читает').entries()) {
    const el = make('span', 'reader');
    el.append(make('i', 'icon'), make('b', '', rd.text));
    const x = into.at(i);
    if (x) el.append(make('span', 'slot into', x.text));
    readers.append(link(role(el, 'prelude'), rd.target, open));
  }
  box.append(writers, pipe, readers);
  return box;
}

const visuals = new Map<
  string,
  (lines: readonly HintLine[], open: Open) => HTMLElement
>([['stream', streamVisual]]);

// Documentation with chips: [from, to] in code points of text - the lines “chip < role>”
function doc(
  text: string,
  chips: readonly HintLine[],
  open: Open,
): HTMLElement {
  const p = make('p', 'doc');
  // eslint-disable-next-line @typescript-eslint/no-misused-spread
  const cps = [...text];
  let at = 0;
  for (const c of [...chips].sort((a, b) => a.from - b.from)) {
    if (c.from < at) continue;
    p.append(cps.slice(at, c.from).join(''));
    const chip = make('span', 'chip');
    chip.append(make('i', 'icon'), cps.slice(c.from, c.to).join(''));
    p.append(link(role(chip, c.kind.slice('чип '.length)), c.target, open));
    at = c.to;
  }
  p.append(cps.slice(at).join(''));
  return p;
}

// Error card (frame “Trace Error”): header, chain of steps with chips with arrows (tone - from pale to the
// place of error), “why” in one line, actions “Add Signature” and “To Call”.
function errorCard(
  lines: readonly HintLine[],
  open: Open,
  host: Host,
): HTMLElement {
  const card = make('div', 'card error');
  const head = make('div', 'head');
  head.append(
    make('i', 'dot'),
    make(
      'span',
      'title',
      lines.find((l) => l.kind === 'ошибка')?.text.split('\n')[0] ?? '',
    ),
  );
  const chain = make('div', 'chain');
  const steps = lines.filter((l) => l.kind.startsWith('шаг'));
  for (const [i, st] of steps.entries()) {
    if (i > 0) chain.append(make('span', 'arrow', '→'));
    const [n = '', name = ''] = st.text.split(' · ');
    const chip = make('span', 'step');
    chip.style.setProperty('--step', String((i + 1) / steps.length));
    chip.toggleAttribute('place', i === steps.length - 1);
    const icon = make('i', 'icon');
    role(icon, st.kind.slice('шаг'.length).trim());
    chip.append(make('b', 'n', n), icon, name);
    role(chip, st.kind.slice('шаг'.length).trim());
    chain.append(link(chip, st.target, open));
  }
  card.append(head, chain);
  const why = (k: string) =>
    lines.find((l) => l.kind.startsWith(`почему ${k}`));
  const arg = why('имя');
  if (arg) {
    const p = make('div', 'why');
    const chip = (text: string, cls: string) =>
      make('span', `chip ${cls}`, text);
    const name = role(
      chip(arg.text, 'arg'),
      arg.kind.slice('почему имя'.length).trim(),
    );
    p.append(
      link(name, arg.target, open),
      ' — ',
      chip(why('тип')?.text ?? '', 'bad'),
    );
    const from = why('из');
    if (from) p.append(' из ', chip(from.text, 'origin'));
    p.append(
      `, а ${why('функция')?.text ?? ''} ждёт `,
      chip(why('ждёт')?.text ?? '', 'want'),
    );
    card.append(p);
  }
  const add = lines.find((l) => l.kind === 'добавить');
  const back = lines.find((l) => l.kind === 'к вызову');
  if (add || back) {
    const acts = make('div', 'actions');
    if (add) {
      const b = make('button', 'add');
      b.append(
        make('i', 'icon plus'),
        'Добавить ',
        make('span', 'sig', add.text),
        make('span', 'fill'),
        make('kbd', '', 'F12'),
      );
      b.addEventListener('mousedown', (e) => {
        e.preventDefault();
        e.stopPropagation();
        host.goto(add.text);
      });
      acts.append(b);
    }
    if (back) {
      const b = make('button', 'back');
      b.append(make('i', 'icon back'), 'К вызову');
      acts.append(link(b, back.target, open));
    }
    card.append(acts);
  }
  return card;
}

export function tip(
  el: HTMLElement,
  lines: readonly HintLine[],
  open: Open,
  host: Host,
): void {
  const name = lines.find((l) => l.kind === 'имя');
  if (!name && lines.some((l) => l.kind === 'ошибка')) {
    el.replaceChildren(errorCard(lines, open, host));
    return;
  }
  if (!name) {
    renderLines(el, lines, open);
    return;
  }
  const card = make('div', 'card');
  const kind = lines.find((l) => l.kind === 'схема')?.text ?? '';
  const scheme = lines.filter((l) => l.kind.startsWith('схема'));
  if (scheme.length > 0) {
    const draw = visuals.get(kind);
    if (draw) card.append(draw(lines, open));
    else {
      const list = make('div', 'visual');
      renderLines(list, scheme, open);
      card.append(list);
    }
  }
  const body = make('div', 'body');
  const sig = lines.find((l) => l.kind === 'сигнатура');
  if (sig) body.append(make('div', 'sig', sig.text));
  body.append(make('div', 'title', name.text));
  const text = lines.find((l) => l.kind === 'документация');
  if (text)
    body.append(
      doc(
        text.text,
        lines.filter((l) => l.kind.startsWith('чип ')),
        open,
      ),
    );
  // Other (type, value, errors, announcements) - lines below
  const shown = new Set(['имя', 'сигнатура', 'документация', 'комментарий']); // doc is already higher
  const rest = lines.filter(
    (l) =>
      !shown.has(l.kind) &&
      !l.kind.startsWith('чип ') &&
      !l.kind.startsWith('схема'),
  );
  if (rest.length > 0) {
    const more = make('div', 'more');
    renderLines(more, rest, open);
    body.append(more);
  }
  card.append(body);
  el.replaceChildren(card);
}
