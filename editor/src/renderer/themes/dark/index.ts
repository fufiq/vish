// The theme “dark” - window on the frame “B · Dark” (author’s decision 2026-10-28: a separate topic, as in the
// frame): one dark card - tabs on top (Module, function under the cursor with a draft point, Layers, Inspector;
// on the right - the type at the cursor), the transition line is always visible, the code, the status bar at
// the bottom (errors, position, draft, start). The basis is the "canvas": its panels (cards, lists, merger,
// track) - with dark colors.
import type { Theme } from '../../theme';
import classicShell from '../classic/shell.html?raw';
import css from './styles.css?inline';

const header = '<header id="header"></header>';
const find =
  '<div id="find-bar"><div class="box" role="button" tabindex="-1"><i class="icon find"></i><span class="text">Перейти: имя или сигнатура — foo(i64)</span><kbd>Ctrl P</kbd></div></div>';
const statusBar = '<footer id="status-bar"></footer>';

const make = (tag: string, className: string, text = ''): HTMLElement => {
  const el = document.createElement(tag);
  el.className = className;
  el.textContent = text;
  return el;
};

const theme: Theme = {
  name: 'dark',
  base: 'canvas',
  css: [css],
  shell: header + find + classicShell + statusBar,
  panels: {
    // tabs and status bar - one summary of the window (HeaderView)
    header(el, h, host) {
      const tab = (text: string, f: () => void, on = false) => {
        const b = make('button', on ? 'tab on' : 'tab', text);
        b.addEventListener('mousedown', (e) => {
          e.preventDefault();
          f();
        });
        return b;
      };
      const fn = h.fn === '' ? [] : [tab(h.fn, () => undefined, true)];
      if (h.draft) fn.at(0)?.append(make('span', 'draft-dot'));
      el.replaceChildren(
        tab('Модуль', () => undefined, h.fn === ''),
        ...fn,
        tab('Слои', () => {
          if (h.fnNode !== null) host.open(h.fnNode); // function in the second panel - its layers
        }),
        tab('Инспектор', () => {
          host.press('F2');
        }),
        make('span', 'fill'),
        ...(h.focus === '' ? [] : [make('span', 'focus', h.focus)]),
      );
      const find = document.getElementById('find-bar');
      if (find)
        find.onmousedown = (e) => {
          e.preventDefault();
          host.press('p', 2);
        };
      const bar = document.getElementById('status-bar');
      if (!bar) return;
      const errors = make(
        'span',
        h.errors > 0 ? 'errors on' : 'errors',
        !h.checked
          ? 'проверяется'
          : h.errors === 0
            ? 'проверено'
            : h.errors === 1
              ? '1 ошибка'
              : `ошибок: ${String(h.errors)}`,
      );
      errors.prepend(make('span', 'dot'));
      const run = make(
        'button',
        'run',
        h.running ? 'Остановить ⇧F5' : 'Запуск F5',
      );
      run.addEventListener('mousedown', (e) => {
        e.preventDefault();
        if (h.running) host.stop();
        else host.run();
      });
      bar.replaceChildren(
        errors,
        ...(h.conflicts > 0
          ? [make('span', 'merge', `слияние · ${String(h.conflicts)} конфл.`)]
          : []),
        make('span', 'fill'),
        make('span', 'at', `${String(h.line)}:${String(h.col)}`),
        ...(h.draft ? [make('span', 'draft', '● черновик')] : []),
        run,
      );
    },
  },
};

export default theme;
