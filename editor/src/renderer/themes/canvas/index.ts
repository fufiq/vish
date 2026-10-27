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
