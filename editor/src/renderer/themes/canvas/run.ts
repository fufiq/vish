// The launch and track of the theme “canvas” (frames “Laptop · Launch” and “Laptop · track”): the header of
// the launch panel with the state and buttons F5 / Shift + F5; the list of track stops with the value of the
// instructions and tags F10 · now · F10, the result of the path; the dark lane of the track control at the
// bottom. Data from VM. Stop/start.
import type { Stops, TraceStep } from '../../../protocol/messages';
import type { Host, RunState } from '../../theme';

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

const states: Record<RunState['state'], string> = {
  idle: 'не запускали',
  running: 'идёт',
  done: 'выполнено',
  error: 'ошибка',
  stopped: 'остановлено',
};

export function runHead(el: HTMLElement, s: RunState, host: Host): void {
  const title = make('span', 'title', 'Запуск');
  const pill = make('span', 'pill', states[s.state]);
  pill.dataset.state = s.state;
  const sub = make(
    'span',
    'sub',
    'процесс машины в этой VM · квантами · вывод по ходу',
  );
  const again = make('button', 'again');
  again.append(make('i', 'icon again'), 'Перезапустить', make('kbd', '', 'F5'));
  press(again, () => {
    host.run();
  });
  const stop = make('button', 'stop');
  stop.append(
    make('i', 'icon stop'),
    'Остановить',
    make('kbd', '', 'Shift+F5'),
  );
  stop.disabled = s.state !== 'running';
  press(stop, () => {
    host.stop();
  });
  el.replaceChildren(
    make('i', 'icon term'),
    title,
    pill,
    sub,
    make('span', 'fill'),
    again,
    stop,
  );
}

export function stops(el: HTMLElement, m: Stops, host: Host): void {
  const head = make('div', 'shead');
  head.append(
    make('span', 'title', 'Трасса'),
    make('span', 'sub', 'остановки шагов · сдвиг — глубина вызова'),
  );
  const hint = make(
    'div',
    'hint',
    'записана при запуске (F6) · щелчок — к шагу',
  );
  const list = make('div', 'list');
  for (const s of m.stops) {
    const row = make('div', 'stop');
    row.toggleAttribute('current', s.step === m.current);
    row.style.setProperty('--depth', String(Math.max(0, s.depth - 1)));
    const tag =
      s.step === m.current
        ? 'сейчас'
        : s.step + 1 === m.prev
          ? '⇧F10'
          : s.step + 1 === m.next
            ? 'F10'
            : '';
    const top = make('div', 'top');
    top.append(
      make('span', 'n', String(s.step + 1)),
      make('i', 'icon fn'),
      make('span', 'fn', s.fn),
    );
    if (tag !== '') top.append(make('span', 'tag', tag));
    const body = make('div', 'body');
    body.append(make('span', 'text', s.text));
    if (s.value !== '') body.append(make('span', 'value', s.value));
    row.append(top, body);
    press(row, () => {
      host.send({ type: 'c_trace_step', step: s.step });
    });
    list.append(row);
  }
  el.replaceChildren(head, hint, list, make('div', 'summary', m.summary));
  list.querySelector('[current]')?.scrollIntoView({ block: 'nearest' });
}

export function traceBar(
  el: HTMLElement,
  t: TraceStep | null,
  host: Host,
): void {
  el.hidden = t === null;
  if (!t) return;
  const path = make('span', 'path');
  [...t.stack].reverse().forEach((f, i) => {
    if (i > 0) path.append(make('span', 'sep', '›'));
    path.append(make('span', 'fn', f));
  });
  const step = make('span', 'step');
  step.append(
    make('span', 'k', 'шаг'),
    make('b', '', `${String(t.step + 1)} / ${String(t.total)}`),
  );
  const out = make('span', 'out');
  const last = t.output.trimEnd().split('\n').at(-1) ?? '';
  out.append(
    make('span', 'k', 'вывод'),
    make('b', '', last === '' ? '—' : last),
  );
  const keys = make('span', 'keys');
  for (const [label, key, shift] of [
    ['⇧F11 наружу', 'F11', 1],
    ['⇧F10 назад', 'F10', 1],
    ['F10 дальше', 'F10', 0],
    ['F11 внутрь', 'F11', 0],
    ['F4 до курсора', 'F4', 0],
  ] as const) {
    const b = make('button', '', label);
    press(b, () => {
      host.key(key, shift);
    });
    keys.append(b);
  }
  const esc = make('button', 'esc', 'Esc');
  press(esc, () => {
    host.key('Escape');
  });
  el.replaceChildren(path, step, out, make('span', 'fill'), keys, esc);
}
