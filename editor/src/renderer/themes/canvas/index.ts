// The theme “canvas” is a window on the frames of the design canvas (“Laptop · Editor”, “Style · entity
// AST”): a light background with blurred spots, a glass card with code, a VISH header · file · checked ·
// position, line numbers, Inter and JetBrains Mono. The basis is classic: panels and markup of it, its
// own - hat, background and styles.
import '@fontsource/inter/400.css';
import '@fontsource/inter/500.css';
import '@fontsource/inter/600.css';
import '@fontsource/jetbrains-mono/400.css';
import '@fontsource/jetbrains-mono/500.css';
import type { Theme } from '../../theme';
import classicShell from '../classic/shell.html?raw';
import { tip } from './card';
import { complete } from './complete';
import { palette } from './palette';
import { expansion, inspect } from './inspector';
import { proposals } from './ai';
import { merge, mergeNotes } from './merge';
import { runHead, stops, traceBar } from './run';
import css from './styles.css?inline';

const backdrop =
  '<div id="backdrop" aria-hidden="true"><i></i><i></i><i></i></div>';
const header = '<header id="header"></header>';

const theme: Theme = {
  name: 'canvas',
  base: 'classic',
  css: [css],
  shell: backdrop + header + classicShell,
  panels: {
    tip,
    complete,
    palette,
    inspect,
    expansion,
    runHead,
    traceBar,
    stops,
    merge,
    mergeNotes,
    proposals,
    // One VM - several clients (frame “Two clients”): you, others (where the cursor is, what rules), explanation
    clients(el, m) {
      el.hidden = m.peers.length === 0;
      const make = (tag: string, className: string, text = '') => {
        const e = document.createElement(tag);
        e.className = className;
        e.textContent = text;
        return e;
      };
      const row = (who: string, letter: string, sub: string, me: boolean) => {
        const r = make('div', me ? 'client me' : 'client');
        const text = make('span', 'text');
        text.append(make('b', '', who), make('span', 'sub', sub));
        r.append(make('span', 'avatar', letter), text);
        return r;
      };
      const head = make('div', 'head');
      head.append(
        make('i', 'icon peers'),
        m.peers.length === 1
          ? 'Одна VM — два клиента'
          : `Одна VM — клиентов: ${String(m.peers.length + 1)}`,
      );
      el.replaceChildren(
        head,
        row('вы', 'В', `курсор в ${m.me}`, true),
        ...m.peers.map((p) => {
          const name =
            p.name === '' ? `клиент ${String(p.client + 1)}` : p.name;
          return row(
            name,
            name.at(0)?.toUpperCase() ?? 'К',
            p.draft === ''
              ? `курсор в ${p.where}`
              : `черновик ${p.draft} — набирает`,
            false,
          );
        }),
        make(
          'div',
          'note',
          'Состояние одно, на сервере. Пока элемент в чужом черновике, ваши правки в нём отклоняются; остальной код правится свободно. Закончит — изменение придёт вам.',
        ),
      );
    },
    // Panel at the cursor (frame "Edit"): expression with type, step and instance from VM; overload (F12), node
    // in the inspector (F2), still - transition by signature (Ctrl+P)
    cursorBar(el, lines, actions) {
      const main = lines.find((l) => l.kind === 'панель');
      el.hidden = !main;
      if (!main) return;
      const span = (className: string, text = '') => {
        const s = document.createElement('span');
        s.className = className;
        s.textContent = text;
        return s;
      };
      const button = (text: string, f: () => void, className = '') => {
        const b = document.createElement('button');
        b.textContent = text;
        b.className = className;
        b.addEventListener('mousedown', (e) => {
          e.preventDefault();
          e.stopPropagation(); // Panel - not the place of installation of the cursor in the code
          f();
        });
        return b;
      };
      const what = span('what', main.text);
      what.prepend(span('dot'));
      const stage = lines.find((l) => l.kind === 'панель этап');
      const add = lines.find((l) => l.kind === 'добавить');
      if (add) what.classList.add('error');
      el.replaceChildren(
        span('arrow'),
        what,
        span('sep'),
        ...(stage ? [span('stage', stage.text)] : []),
        // No overload - "Add Signature" (palette transition from "Add"), otherwise - overload
        add
          ? button(
              `Добавить ${add.text}`,
              () => {
                actions.goto(add.text);
              },
              'strong add',
            )
          : button(
              'Перегрузки +',
              () => {
                actions.overloads();
              },
              'strong',
            ),
        button('Узел', () => {
          actions.inspect();
        }),
        span('sep'),
        button('⋯', () => {
          actions.goto();
        }),
      );
    },
    // panel cap (frame "projection"): the same program next to it - which is projection, edit - its records
    paneHead(el, p) {
      el.hidden = !p.twin;
      const key = document.createElement('span');
      key.className = p.projection === '' ? 'key default' : 'key';
      key.textContent = p.projection === '' ? 'по умолчанию' : p.projection;
      const note = document.createElement('span');
      note.className = 'note';
      note.textContent =
        p.projection === ''
          ? p.active
            ? 'активна'
            : ''
          : `${p.active ? 'активна · ' : ''}правка разбирается её записями`;
      el.replaceChildren(key, note);
      el.toggleAttribute('active', p.active);
    },
    // number - only the lines with the code; empty and comments - without a number, counting (frame "Laptop")
    gutter(el, lines) {
      el.replaceChildren(
        ...lines.map((l, i) => {
          const d = document.createElement('div');
          d.textContent = l.code ? String(i + 1) : '\u00a0'; // empty div would collapse
          d.toggleAttribute('draft', l.draft); // draft
          d.toggleAttribute('conflict', l.conflict); // fusion
          if (l.step === '') d.removeAttribute('step');
          else d.setAttribute('step', l.step); // step of the track - the whole line (frame "Trace")
          if (l.path > 0) d.dataset.path = l.path === 2 ? 'place' : 'step'; // line
          if (l.layer !== '') d.dataset.layer = l.layer; // Instruction overload: General or Own
          d.toggleAttribute('running', l.running); // here is the launch.
          return d;
        }),
      );
    },
    header(el, h) {
      const span = (className: string, text = '') => {
        const s = document.createElement('span');
        s.className = className;
        s.textContent = text;
        return s;
      };
      const check = span(
        'check',
        !h.checked
          ? 'проверяется'
          : h.errors === 0
            ? 'проверено'
            : `ошибок: ${String(h.errors)}`,
      );
      check.dataset.state = !h.checked
        ? 'busy'
        : h.errors === 0
          ? 'ok'
          : 'error';
      check.prepend(span('dot'));
      if (h.conflicts > 0) {
        // “Merge”: while conflicts are not resolved, they are the main thing
        check.dataset.state = 'error';
        check.replaceChildren(
          span('dot'),
          `слияние · ${String(h.conflicts)} ${h.conflicts === 1 ? 'конфликт' : h.conflicts < 5 ? 'конфликта' : 'конфликтов'}`,
        );
      }
      if (h.running) check.append(' · ', span('running', '▶ идёт')); // "Run" shot.
      el.replaceChildren(
        span('brand', 'VISH'),
        span('file', h.file),
        span('fill'),
        check,
        span('at', `${String(h.line)}:${String(h.col)}`),
      );
    },
  },
};

export default theme;
