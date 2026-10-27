// The theme “classic” is basic: window markup and all panels. Other themes take it as a basis
// (base: 'classic') and replace theirs - or ask everything themselves.
import type { Theme } from '../../theme';
import { renderComplete, renderLines } from '../../view';
import css from './styles.css?inline';
import shell from './shell.html?raw';

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

const press = (el: HTMLElement, f: () => void): void => {
  el.addEventListener('mousedown', (e) => {
    e.preventDefault();
    f();
  });
};

// text with modified [from, to] (code points); empty - "no -"
const marked = (text: string, from: number, to: number): HTMLPreElement => {
  const pre = make('pre');
  // eslint-disable-next-line @typescript-eslint/no-misused-spread
  const cps = [...text];
  if (text === '') pre.textContent = '— нет —';
  else if (to > from)
    pre.append(
      cps.slice(0, from).join(''),
      make('mark', '', cps.slice(from, to).join('')),
      cps.slice(to).join(''),
    );
  else pre.textContent = text;
  return pre;
};

const theme: Theme = {
  name: 'classic',
  css: [css],
  shell,
  panels: {
    // Brush status - a black capsule on top: mode and most importantly; "" - hide (text goes out with
    // the capsule). Button (frame "Service States"): "goto signature" - to the transition list,
    // "reconnect" - to connect to the VM again.
    status(el, s, host) {
      el.setAttribute('kind', s.kind);
      el.hidden = s.kind === '';
      if (s.kind === '') return;
      const text = el.querySelector('#status-text');
      const hint = el.querySelector('#status-hint');
      const action = el.querySelector<HTMLButtonElement>('#status-action');
      if (text) text.textContent = s.text;
      if (hint) hint.textContent = s.hint;
      if (!action) return;
      const act = s.action ?? '';
      action.hidden = act === '';
      action.textContent = s.label ?? '';
      action.onmousedown = (e) => {
        e.preventDefault();
        if (act === 'reconnect') host.reconnect();
        else if (act.startsWith('goto ')) host.goto(act.slice(5));
        else if (act.startsWith('open ')) host.open(Number(act.slice(5))); // "To Mistake"
      };
    },

    // Function layers, two levels: congestion switch ‹ head i/n › (click - c_open other overload) and
    // context tabs selected: "all" and its instance (compile-time / run-time).
    layers(el, m, host) {
      const { view, layers, selected, overloads, overload } = m;
      const pick = (k: number) => {
        const fn = overloads.at(k % overloads.length)?.fn ?? null;
        if (fn !== null) host.send({ type: 'c_open', view, node: fn });
      };
      const arrow = (label: string, k: number) => {
        const b = make('button', 'arrow', label);
        press(b, () => {
          pick(k);
        });
        return b;
      };
      const switcher: HTMLElement[] = [];
      if (overloads.length > 1)
        switcher.push(
          arrow('‹', overload - 1),
          make(
            'span',
            'overload',
            `${overloads[overload]?.head ?? ''}  ${String(overload + 1)}/${String(overloads.length)}`,
          ),
          arrow('›', overload + 1),
          make('span', 'sep'),
        );
      const tab = (key: string, label: string, kind: string) => {
        const b = make('button', '', label);
        if (kind !== '') b.dataset.kind = kind;
        b.toggleAttribute('selected', key === selected);
        press(b, () => {
          host.send({ type: 'c_layer', view, key });
        });
        return b;
      };
      el.replaceChildren(
        ...switcher,
        tab('', 'все', ''),
        ...layers.map((l) => tab(l.key, l.key, l.kind)),
      );
    },

    // Other clients: each has where the cursor is and what rules (frame
    // "Two clients"); no one - no panel.
    clients(el, m) {
      el.hidden = m.peers.length === 0;
      el.replaceChildren(
        make(
          'div',
          'head',
          `Одна VM — клиентов: ${String(m.peers.length + 1)}`,
        ),
        ...m.peers.map((p) => {
          const row = make('div', 'client');
          const text = make('span');
          text.append(
            make(
              'b',
              '',
              p.name === '' ? `клиент ${String(p.client + 1)}` : p.name,
            ),
            make(
              'span',
              '',
              p.draft === ''
                ? `курсор: ${p.where}`
                : `черновик ${p.draft} — набирает`,
            ),
          );
          row.append(
            make('span', 'avatar', p.name === '' ? 'К' : (p.name.at(0) ?? 'К')),
            text,
          );
          return row;
        }),
      );
    },

    // Track stops: call depth - shift and guides, current highlighted; click - to step (VM). The window
    // around the step sends VM.
    stops(el, m, host) {
      const list = make('div', 'list');
      for (const s of m.stops) {
        const row = make('div', 'stop');
        row.toggleAttribute('current', s.step === m.current);
        row.style.setProperty('--depth', String(Math.max(0, s.depth - 1)));
        row.append(
          make('span', 'n', String(s.step + 1)),
          make('span', 'fn', s.fn),
          make('span', 'text', s.text),
        );
        press(row, () => {
          host.send({ type: 'c_trace_step', step: s.step });
        });
        list.append(row);
      }
      el.replaceChildren(make('div', 'head', 'остановки'), list);
      list.querySelector('[current]')?.scrollIntoView({ block: 'nearest' });
    },

    // Launch output (frame "Run"): a line - with the signature of your print ("main:12", click - to
    // call). Lines without space (error, run by interpreter) - text.
    output(el, rows, host) {
      if (rows.length === 0) {
        el.textContent = '(вывода нет)';
        return;
      }
      el.replaceChildren(
        ...rows.map((r) => {
          const row = make('div', 'out-row');
          const label = make('span', 'out-at', r.label);
          const node = r.node;
          if (node !== null) {
            label.classList.add('link');
            press(label, () => {
              host.open(node);
            });
          }
          row.append(label, make('span', '', r.text));
          return row;
        }),
      );
    },

    // Proposals of edits (AI): element, who and why, current text and proposed with a marked difference;
    // "Accept" - one edit, "Reject" - remove (decides VM).
    proposals(el, ps, host) {
      el.hidden = ps.length === 0;
      el.replaceChildren(
        ...ps.map((p) => {
          const col = (
            name: string,
            text: string,
            from: number,
            to: number,
          ) => {
            const c = make('div', 'side');
            c.append(make('div', 'head', name), marked(text, from, to));
            return c;
          };
          const cols = make('div', 'cols');
          cols.append(
            col('сейчас', p.base, 0, 0),
            col(`предложено · ${p.note}`, p.text, p.text_from, p.text_to),
          );
          const button = (label: string, accept: boolean) => {
            const b = make('button', '', label);
            b.toggleAttribute('accept', accept);
            press(b, () => {
              host.send({ type: 'c_decide', id: p.id, accept });
            });
            return b;
          };
          const buttons = make('div', 'buttons');
          buttons.append(button('Принять', true), button('Отклонить', false));
          const box = make('div', 'proposal');
          box.append(
            make('div', 'title', `${p.from} предлагает: ${p.key} — ${p.why}`),
            cols,
            buttons,
          );
          return box;
        }),
      );
    },

    // Merger (Ctrl+M): Conflicts are the base, mine and them next to the difference; "take them" or
    // "leave mine."
    merge(el, conflicts, host) {
      el.hidden = conflicts.length === 0; // The result is in a bang from VM.
      el.replaceChildren(
        ...conflicts.map((c) => {
          const side = (
            name: string,
            text: string,
            note: string,
            from: number,
            to: number,
            take?: { label: string; theirs: boolean },
          ) => {
            const col = make('div', 'side');
            const head = make('div', 'head');
            head.append(make('b', '', name), make('span', '', note));
            col.append(head, marked(text, from, to));
            if (take) {
              const btn = make('button', '', take.label);
              press(btn, () => {
                host.send({ type: 'c_take', key: c.key, theirs: take.theirs });
              });
              col.append(btn);
            }
            return col;
          };
          const cols = make('div', 'cols');
          cols.append(
            side('база', c.base, 'общее начало', 0, 0),
            side('мой', c.mine, c.mine_note, c.mine_from, c.mine_to, {
              label: 'Оставить мой',
              theirs: false,
            }),
            side('их', c.theirs, c.theirs_note, c.theirs_from, c.theirs_to, {
              label: 'Взять их',
              theirs: true,
            }),
          );
          const box = make('div', 'conflict');
          box.append(make('div', 'title', `конфликт: ${c.key}`), cols);
          return box;
        }),
      );
    },

    // In the classic hat and line numbers are not (there is no #header markup; the numbers are hidden by style)
    header() {
      return;
    },
    tip: renderLines,
    complete: renderComplete,
    gutter(el, lines) {
      el.replaceChildren(
        ...lines.map((_, i) => make('div', '', String(i + 1))),
      );
    },
  },
};

export default theme;
