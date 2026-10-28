import { describe, expect, it } from 'vitest';
import { resolve, type Panels, type Theme } from './theme';

const noop = () => undefined;
const panels: Panels = {
  status: noop,
  layers: noop,
  clients: noop,
  stops: noop,
  output: noop,
  proposals: noop,
  merge: noop,
  header: noop,
  gutter: noop,
  tip: noop,
  complete: noop,
  palette: noop,
  inspect: noop,
  expansion: noop,
  runHead: noop,
  traceBar: noop,
};

describe('темы', () => {
  it('тема берёт недостающее из базовой, своё — поверх', () => {
    const header = () => undefined;
    const themes = new Map<string, Theme>([
      ['base', { name: 'base', css: ['a{}'], shell: '<div id="app">', panels }],
      [
        'mine',
        { name: 'mine', base: 'base', css: ['b{}'], panels: { header } },
      ],
    ]);
    const r = resolve(themes, 'mine');
    expect(r.css).toEqual(['a{}', 'b{}']);
    expect(r.shell).toBe('<div id="app">');
    expect(r.panels.header).toBe(header);
    expect(r.panels.status).toBe(noop);
  });

  it('тема без базовой обязана задать разметку и все панели', () => {
    const themes = new Map<string, Theme>([
      ['half', { name: 'half', panels: { status: noop } }],
    ]);
    expect(() => resolve(themes, 'half')).toThrow(/shell, layers/);
    expect(() => resolve(themes, 'нет')).toThrow(/нет темы/);
  });
});
