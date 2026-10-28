import {
  beforeAll,
  beforeEach,
  afterEach,
  describe,
  expect,
  it,
  vi,
} from 'vitest';
import { decode, frame, type Value } from '../protocol/codec';
import {
  schema,
  type ClientMsg,
  type Hover,
  type ServerMsg,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';

let receive: (bytes: Uint8Array) => void;
let aiEvent: (kind: string, text: string) => void;
const sent: ClientMsg[] = [];
const stop = vi.fn();

function emit(m: ServerMsg): void {
  receive(frame(schema, schema.server, m as unknown as Value));
}
function element(selector: string): HTMLElement {
  const el = document.querySelector<HTMLElement>(selector);
  if (!el) throw new Error(`нет ${selector}`);
  return el;
}
function key(el: HTMLElement, value: string, ctrlKey = false): void {
  el.dispatchEvent(
    new KeyboardEvent('keydown', {
      key: value,
      ctrlKey,
      bubbles: true,
      cancelable: true,
    }),
  );
}
function pressBar(text: string): void {
  const b = [
    ...document.querySelectorAll<HTMLButtonElement>('.vish-bar button'),
  ].find((x) => x.textContent === text);
  if (!b) throw new Error(`нет кнопки ${text}`);
  b.dispatchEvent(
    new MouseEvent('mousedown', { button: 0, bubbles: true, cancelable: true }),
  );
}

beforeAll(async () => {
  const bridge: VmBridge = {
    send(bytes) {
      sent.push(
        decode(
          schema,
          schema.client,
          bytes.subarray(4),
        ) as unknown as ClientMsg,
      );
    },
    onData(cb) {
      receive = cb;
    },
    onAi(cb) {
      aiEvent = cb;
    },
    onReady: () => undefined,
    onStatus: () => undefined,
    reconnect: () => undefined,
    aiAsk: () => undefined,
    aiStop: stop,
  };
  window.vm = bridge;
  // jsdom does not decompose text; here we check the routing of events and messages.
  Range.prototype.getBoundingClientRect = () => new DOMRect();
  Element.prototype.getClientRects = () =>
    Object.assign([new DOMRect()], {
      item: (i: number) => (i === 0 ? new DOMRect() : null),
    });
  Element.prototype.scrollIntoView = () => undefined;
  await import('./index');
});

beforeEach(() => {
  vi.useFakeTimers();
  emit({
    type: 's_view',
    view: 1,
    root: 10,
    revision: 1,
    kind: 'module',
    projection: '',
    nodes: [
      { type: 's_node', id: 10, kind: 'n_module', pieces: [11], note: '' },
      { type: 's_node', id: 11, kind: 'n_call', pieces: ['foo()'], note: '' },
    ],
  });
  emit({
    type: 's_view',
    view: 2,
    root: 20,
    revision: 1,
    kind: 'fn',
    projection: '',
    nodes: [
      {
        type: 's_node',
        id: 20,
        kind: 'n_fn',
        pieces: ['other() {}'],
        note: '',
      },
    ],
  }); // another species active, cursor not in foo
  element('#ast').hidden = true;
  element('#ai').hidden = true;
  sent.length = 0;
  stop.mockClear();
  aiEvent('done', '');
});
afterEach(() => {
  vi.clearAllTimers();
  vi.useRealTimers();
});

const hover: Hover = {
  type: 's_hover',
  view: 1,
  node: 11,
  lines: [
    {
      type: 's_line',
      kind: 'панель',
      text: 'foo() : i64',
      target: 11,
      from: 0,
      to: 0,
    },
  ],
};
function menu(): void {
  element('#app [data-id="11"]').dispatchEvent(
    new MouseEvent('contextmenu', {
      button: 2,
      bubbles: true,
      composed: true,
      cancelable: true,
    }),
  );
  emit(hover);
  sent.length = 0;
}

describe('контекстные действия окна', () => {
  it('«Узел» инспектирует узел ПКМ в его представлении, а не старый курсор другой панели', () => {
    menu();
    pressBar('Узел');
    vi.advanceTimersByTime(200);
    expect(sent).toEqual([{ type: 'c_inspect', view: 1, node: 11 }]);
    expect(element('#ast').hidden).toBe(false);
    expect(document.activeElement).toBe(element('#app'));
  });
  it('перегрузки ставят место выбранного узла перед F12 и не посылают лишний pointer от кнопки', () => {
    menu();
    pressBar('Перегрузки +');
    expect(sent).toEqual([
      { type: 'c_pointer', view: 1, node: 11, piece: 0, offset: 0 },
      { type: 'c_key', view: 1, key: 'F12', mods: 0 },
    ]);
  });
  it('панель инспектора использует тот же узел для перехода', () => {
    emit({
      type: 's_inspect',
      view: 1,
      root: 10,
      focus: 11,
      nodes: [
        {
          type: 's_inode',
          id: 11,
          depth: 0,
          kind: 'ast.call',
          role: '',
          text: 'foo()',
          rule: '',
          rule_at: null,
          types: '',
        },
      ],
      expand: [],
    });
    element('#ast .row').dispatchEvent(
      new MouseEvent('contextmenu', {
        button: 2,
        bubbles: true,
        clientX: 10,
        clientY: 10,
        cancelable: true,
      }),
    );
    emit(hover);
    sent.length = 0;
    pressBar('⋯');
    expect(sent).toEqual([
      { type: 'c_pointer', view: 1, node: 11, piece: 0, offset: 0 },
    ]);
    expect(element('#goto').hidden).toBe(false);
    key(element('#goto-input'), 'Escape');
  });
  it('добавление из панели привязано к выбранному представлению', () => {
    element('#app [data-id="11"]').dispatchEvent(
      new MouseEvent('contextmenu', {
        button: 2,
        bubbles: true,
        composed: true,
        cancelable: true,
      }),
    );
    emit({
      ...hover,
      lines: [
        ...hover.lines,
        {
          type: 's_line',
          kind: 'добавить',
          text: 'foo(i64)',
          target: null,
          from: 0,
          to: 0,
        },
      ],
    });
    sent.length = 0;
    pressBar('Добавить foo(i64)');
    expect(sent).toEqual([
      { type: 'c_pointer', view: 1, node: 11, piece: 0, offset: 0 },
      { type: 'c_goto', view: 1, text: 'foo(i64)' },
    ]);
  });
});

describe('клавиатура панели ИИ', () => {
  it.each(['i', 'ш'])(
    'Ctrl+%s закрывает панель из поля и возвращает фокус',
    (value) => {
      key(element('#side'), 'i', true);
      expect(document.activeElement).toBe(element('#ai-input'));
      key(element('#ai-input'), value, true);
      expect(element('#ai').hidden).toBe(true);
      expect(document.activeElement).toBe(element('#side'));
    },
  );
  it('Escape в покое закрывает панель, во время запроса только останавливает его', () => {
    key(element('#side'), 'i', true);
    key(element('#ai-input'), 'Escape');
    expect(element('#ai').hidden).toBe(true);
    key(element('#side'), 'i', true);
    (element('#ai-input') as HTMLTextAreaElement).value = 'вопрос';
    (element('#ai-form') as HTMLFormElement).requestSubmit();
    key(element('#ai-input'), 'Escape');
    expect(stop).toHaveBeenCalledOnce();
    expect(element('#ai').hidden).toBe(false);
  });
});
