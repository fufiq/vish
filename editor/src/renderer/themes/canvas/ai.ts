// Proposals of the agent theme "canvas" (panel "AI", author's decision 2026-10-28: in journal AI and in code):
// a card in the AI card above the field - who and why, "now" and "proposed" with a difference, "Accept" /
// "Deflect". The panel is closed (agent outside, via MCP) - the panel opens.
// - the man decides.
import type { Proposal } from '../../../protocol/messages';
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

export function proposals(
  el: HTMLElement,
  ps: readonly Proposal[],
  host: Host,
): void {
  // place - journal panel "AI", in front of the input field
  const ai = document.getElementById('ai');
  const form = document.getElementById('ai-form');
  if (ai && form && el.parentElement !== ai) ai.insertBefore(el, form);
  el.hidden = ps.length === 0;
  if (ai && ps.length > 0) ai.hidden = false;
  el.replaceChildren(
    ...ps.map((p) => {
      const box = make('div', 'proposal');
      const head = make('div', 'phead');
      head.append(
        make('span', 'who', p.from),
        make('span', '', 'предлагает'),
        make('code', 'key', p.key),
      );
      const cols = make('div', 'cols');
      const now = make('div', 'side');
      now.dataset.side = 'now';
      now.append(make('div', 'label', 'исходная версия'), marked(p.base, 0, 0));
      const next = make('div', 'side');
      next.dataset.side = 'next';
      next.append(
        make('div', 'label', `предложено · ${p.note}`),
        marked(p.text, p.text_from, p.text_to),
      );
      cols.append(now, next);
      const buttons = make('div', 'buttons');
      for (const [label, accept] of [
        ['Отклонить', false],
        ['Принять', true],
      ] as const) {
        const b = make('button', accept ? 'accept' : '', label);
        b.addEventListener('mousedown', (e) => {
          e.preventDefault();
          host.send({ type: 'c_decide', id: p.id, accept });
        });
        buttons.append(b);
      }
      box.append(head);
      if (p.why !== '') box.append(make('div', 'why', p.why));
      box.append(cols);
      if (p.checks !== '') box.append(make('pre', 'why verification', p.checks));
      box.append(buttons);
      return box;
    }),
  );
}
