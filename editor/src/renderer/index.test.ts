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
const ask = vi.fn();

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
    aiAsk: ask,
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
  ask.mockClear();
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

describe('точный контекст ИИ', () => {
  function requestContext(): Extract<ClientMsg, { type: 'c_context' }> {
    key(element('#side'), 'i', true);
    emit({ type: 's_cursor', view: 2, node: 20, piece: 0, offset: 0 });
    (element('#ai-input') as HTMLTextAreaElement).value = 'объясни';
    (element('#ai-form') as HTMLFormElement).requestSubmit();
    const request = sent.findLast((m) => m.type === 'c_context');
    if (request?.type !== 'c_context') throw new Error('нет запроса контекста');
    return request;
  }
  it('запрашивает точный узел и передаёт подтверждённую VM ревизию', () => {
    const request = requestContext();
    expect(request).toMatchObject({ type: 'c_context', view: 2, node: 20 });
    expect(ask).not.toHaveBeenCalled();
    emit({
      type: 's_agent',
      request: request.request + 1,
      revision: 10,
      workspace: 0,
      proposal: 0,
      ok: true,
      text: 'чужой ответ',
    });
    expect(ask).not.toHaveBeenCalled();
    emit({
      type: 's_agent',
      request: request.request,
      revision: 42,
      workspace: 0,
      proposal: 0,
      ok: true,
      text: 'элемент other(i64); точный контекст',
    });
    expect(ask).toHaveBeenCalledWith(expect.stringContaining('ревизия 42'));
    expect(ask).toHaveBeenCalledWith(expect.stringContaining('other(i64)'));
  });
  it('отмена не запускает агента поздним ответом контекста', () => {
    const request = requestContext();
    key(element('#ai-input'), 'Escape');
    emit({
      type: 's_agent',
      request: request.request,
      revision: 42,
      workspace: 0,
      proposal: 0,
      ok: true,
      text: 'поздний ответ',
    });
    expect(ask).not.toHaveBeenCalled();
    expect(element('#ai-form').hasAttribute('busy')).toBe(false);
  });
  it('таймаут контекста завершает ожидание с ошибкой', () => {
    requestContext();
    vi.advanceTimersByTime(20000);
    expect(ask).not.toHaveBeenCalled();
    expect(element('#ai-form').hasAttribute('busy')).toBe(false);
    expect(element('#ai .log').textContent).toContain('VM не ответила');
  });
});

describe('панель «Репозиторий»', () => {
  const info = (request: number, revision: number): ServerMsg => ({
    type: 's_repository',
    request,
    ok: true,
    revision,
    path: '/p.vishrepo',
    branch: 'main',
    head: 0,
    dirty: false,
    message: '',
    diff: '',
    entries: [],
  });
  const repoAsks = () => sent.filter((m) => m.type === 'c_repository');
  beforeEach(() => {
    emit({
      type: 's_hello',
      version: schema.version,
      program: 'p',
      revision: 1,
    });
    // Responding to info connections; waiting for a past test may have left unclosed n
    const asked = repoAsks().at(-1);
    if (asked?.type === 'c_repository') emit(info(asked.request, 3));
    sent.length = 0;
  });

  it('Ctrl+Shift+G открывает панель с запросом info, Escape закрывает без утечки в код', () => {
    const repo = element('#repo');
    repo.hidden = true;
    const e = new KeyboardEvent('keydown', {
      key: 'G',
      ctrlKey: true,
      shiftKey: true,
      bubbles: true,
      cancelable: true,
    });
    element('#side').dispatchEvent(e);
    expect(repo.hidden).toBe(false);
    const asked = repoAsks().at(-1);
    expect(asked).toMatchObject({ action: 'info', revision: 0 });
    if (asked?.type === 'c_repository') emit(info(asked.request, 3));
    const before = sent.length;
    key(repo, 'a');
    key(repo, 'Escape');
    expect(repo.hidden).toBe(true);
    expect(sent.slice(before).some((m) => m.type === 'c_key')).toBe(false);
  });

  it('Ctrl+S при открытом репозитории: info, затем save с присланной ревизией', () => {
    key(element('#side'), 's', true);
    const infoAsk = repoAsks().at(0);
    if (infoAsk?.type !== 'c_repository') throw new Error('нет info');
    expect(infoAsk.action).toBe('info');
    emit(info(infoAsk.request, 4));
    expect(repoAsks().at(-1)).toMatchObject({ action: 'save', revision: 4 });
  });
});
