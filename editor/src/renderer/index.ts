// Editor window: "graffonium" - draws the sent VM and sends it control signals
// (keys, pointer): . On the left - the representation of the module, on the right -
// the function open by transition (F12, Ctrl + P); the keys receive the active
// panel.
//
import { FrameSplitter } from '../protocol/codec';
import {
  Mods,
  decodeServer,
  encodeClient,
  schema,
  type ClientMsg,
  type NodeId,
  type ServerMsg,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';
import { ViewElement } from './view';
import { Inspector } from './inspector';
import './nodes/index';
import './styles.css';

declare global {
  interface Window {
    vm: VmBridge;
  }
}

function byId(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`нет элемента #${id}`);
  return el;
}

const app = byId('app');
const side = byId('side');
const sideWrap = byId('side-wrap');
const layersBar = byId('layers');
const gotoForm = byId('goto') as HTMLFormElement;
const gotoInput = byId('goto-input') as HTMLInputElement;
const mergePanel = byId('merge');
let gotoMode: 'goto' | 'merge' | 'projection' = 'goto';
const statusLine = byId('status');
const runPanel = byId('run');
const runStdin = byId('run-stdin') as HTMLTextAreaElement;
const runArgs = byId('run-args') as HTMLInputElement;
const runOutput = byId('run-output');
const views = new Map<number, ViewElement>();
let active: ViewElement | undefined;
let debugging = false; // Open track: Escape - VM (exit)

function send(m: ClientMsg): void {
  window.vm.send(encodeClient(m));
}

// Inspector AST (F2 - wood panel, Ctrl+Shift+A - frame node in the text): the element tree
// under the cursor, places node and by whom they are disassembled - from VM.
const panes = byId('panes');
const inspector = new Inspector(byId('ast'), {
  open(v, node) {
    send({ type: 'c_open', view: v.data.view, node });
  },
  place(v, node) {
    // cursor - at the beginning of its first text node (or its first descendant)
    const first = (id: NodeId): { node: NodeId; piece: number } | undefined => {
      const n = v.nodes.get(id);
      if (!n) return undefined;
      for (const [i, p] of n.pieces.entries()) {
        if (typeof p === 'string') {
          if (p !== '') return { node: id, piece: i };
        } else {
          const got = first(p);
          if (got) return got;
        }
      }
      return undefined;
    };
    const at = first(node);
    if (at) send({ type: 'c_pointer', view: v.data.view, ...at, offset: 0 });
    activate(v);
  },
});
const cursorNode = new Map<number, NodeId>();
let inspectTimer: ReturnType<typeof setTimeout> | undefined;
function structure(): boolean {
  return panes.classList.contains('structure');
}
function requestInspect(): void {
  if (!inspector.open && !structure()) return;
  clearTimeout(inspectTimer);
  inspectTimer = setTimeout(() => {
    const v = active;
    const node = v ? cursorNode.get(v.data.view) : undefined;
    if (v && node !== undefined)
      send({ type: 'c_inspect', view: v.data.view, node });
  }, 120);
}

function activate(v: ViewElement | undefined): void {
  active = v;
  (v?.host ?? app).focus();
}

function closeSide(): void {
  const v = [...views.values()].find((x) => x.host === side);
  if (v) {
    send({ type: 'c_close_view', view: v.data.view });
    views.delete(v.data.view);
  }
  sideWrap.hidden = true;
  activate([...views.values()].find((x) => x.host === app));
}

function receive(m: ServerMsg): void {
  switch (m.type) {
    case 's_hello':
      statusLine.textContent = `${m.program} · ревизия ${String(m.revision)}`;
      send({ type: 'c_open_view', kind: 'module' });
      return;
    case 's_view': {
      const host = m.kind === 'fn' ? side : app;
      sideWrap.hidden = sideWrap.hidden && host !== side;
      const v = new ViewElement(host, m);
      views.set(m.view, v);
      activate(v);
      return;
    }
    case 's_update':
      views.get(m.view)?.update(m);
      requestInspect();
      statusLine.textContent = `ревизия ${String(m.revision)}`;
      return;
    case 's_cursor':
      views.get(m.view)?.showCursor(m);
      if (m.node !== null) cursorNode.set(m.view, m.node);
      requestInspect();
      return;
    case 's_inspect': {
      const v = views.get(m.view);
      if (v) inspector.show(v, m);
      return;
    }
    case 's_decor': {
      const error = views.get(m.view)?.showMarks(m);
      if (views.get(m.view) !== active || debugging) return; // track-line
      statusLine.textContent =
        error === undefined
          ? `ревизия ${String(m.revision)}`
          : `ошибка: ${error.split('\n')[0] ?? ''}`;
      statusLine.toggleAttribute('error', error !== undefined);
      return;
    }
    case 's_layers':
      showLayers(m.view, m.layers, m.selected);
      return;
    case 's_complete':
      views.get(m.view)?.showCompletion(m);
      return;
    case 's_output':
      runOutput.textContent = m.text === '' ? '(вывода нет)' : m.text;
      runOutput.toggleAttribute('error', !m.ok);
      statusLine.textContent = m.ok ? 'выполнено' : 'ошибка при исполнении';
      return;
    case 's_trace':
      debugging = m.active;
      if (!m.active) {
        statusLine.textContent = 'трасса закрыта';
        return;
      }
      runPanel.hidden = false;
      runOutput.textContent = m.output + (m.error === '' ? '' : `\n${m.error}`);
      runOutput.toggleAttribute('error', m.error !== '');
      statusLine.textContent = `шаг ${String(m.step + 1)} из ${String(m.total)} · ${m.stack.join(' ← ')} · F10 дальше, Shift+F10 назад, F11 внутрь, Shift+F11 наружу, F4 до курсора, Escape — выйти`;
      return;
    case 's_merge':
      showMerge(m.taken, m.conflicts);
      return;
    case 's_saved':
      statusLine.textContent = `сохранено: ${m.path}`;
      return;
    case 's_error':
      statusLine.textContent = `VM: ${m.text}`;
      return;
    case 's_select':
      views.get(m.view)?.showSelection(m);
      return;
    case 's_hover': {
      const v = views.get(m.view);
      if (v && hoverAt)
        v.showHover(m, hoverAt.x, hoverAt.y, (node) => {
          v.hideHover();
          send({ type: 'c_open', view: v.data.view, node });
        });
      return;
    }
    case 's_hint': {
      const v = views.get(m.view);
      v?.showHint(m, (node) => {
        send({ type: 'c_open', view: v.data.view, node });
      });
      return;
    }
    case 's_clipboard':
      navigator.clipboard.writeText(m.text).catch((e: unknown) => {
        statusLine.textContent = `буфер обмена: ${String(e)}`;
      });
      return;
  }
}

// Tabs of the layers of the function: "all" and contexts of its use (compile-time / run-time).
function showLayers(
  view: number,
  layers: readonly { key: string; kind: string }[],
  selected: string,
): void {
  const tab = (key: string, label: string, kind: string) => {
    const b = document.createElement('button');
    b.textContent = label;
    if (kind !== '') b.dataset.kind = kind;
    b.toggleAttribute('selected', key === selected);
    b.addEventListener('mousedown', (e) => {
      e.preventDefault();
      send({ type: 'c_layer', view, key });
    });
    return b;
  };
  layersBar.replaceChildren(
    tab('', 'все', ''),
    ...layers.map((l) => tab(l.key, l.key, l.kind)),
  );
}

const split = new FrameSplitter();
window.vm.onData((bytes) => {
  split.push(bytes, (body) => {
    receive(decodeServer(body));
  });
});
window.vm.onStatus((text) => {
  statusLine.textContent = text;
});
window.vm.onReady(() => {
  send({ type: 'c_hello', version: schema.version });
});

// F5 - start the current state: a panel with stdin and arguments (the window
// remembers them - an unimportant state) and output; Ctrl + S - save the base.
function trace(): void {
  runPanel.hidden = false;
  runOutput.textContent = '…';
  send({ type: 'c_trace', stdin_text: runStdin.value, args: runArgs.value });
}

function run(): void {
  runPanel.hidden = false;
  runOutput.textContent = '…';
  send({ type: 'c_run', stdin_text: runStdin.value, args: runArgs.value });
}
for (const field of [runStdin, runArgs] as HTMLElement[]) {
  field.addEventListener('keydown', (e) => {
    if (e.key === 'F5') {
      e.preventDefault();
      run();
    }
    if (e.key === 'Escape') activate(active);
  });
}

// Merger (Ctrl+M): Conflicts are both options nearby; take them or leave mine.
//
//
function showMerge(
  taken: readonly string[],
  conflicts: readonly { key: string; mine: string; theirs: string }[],
): void {
  if (taken.length > 0)
    statusLine.textContent = `взято у них: ${taken.join(', ')}`;
  mergePanel.hidden = conflicts.length === 0;
  if (conflicts.length === 0 && taken.length === 0)
    statusLine.textContent = 'слито';
  mergePanel.replaceChildren(
    ...conflicts.map((c) => {
      const box = document.createElement('div');
      box.className = 'conflict';
      const title = document.createElement('div');
      title.className = 'title';
      title.textContent = `конфликт: ${c.key}`;
      const side = (label: string, text: string, theirs: boolean) => {
        const col = document.createElement('div');
        const head = document.createElement('button');
        head.textContent = label;
        head.addEventListener('mousedown', (e) => {
          e.preventDefault();
          send({ type: 'c_take', key: c.key, theirs });
        });
        const pre = document.createElement('pre');
        pre.textContent = text === '' ? '— нет —' : text;
        col.append(head, pre);
        return col;
      };
      box.append(
        title,
        side('оставить мой', c.mine, false),
        side('взять их', c.theirs, true),
      );
      return box;
    }),
  );
}

// Ctrl+P is the signature string; it is sent by Enter, Escape is closed.
function openGoto(mode: 'goto' | 'merge' | 'projection' = 'goto'): void {
  gotoMode = mode;
  gotoInput.placeholder =
    mode === 'goto'
      ? 'перейти или добавить: foo(i64)'
      : mode === 'merge'
        ? 'слить с базой: путь.tzdb'
        : 'проекция: ключ модуля записей (пусто — по умолчанию)';
  gotoForm.hidden = false;
  gotoInput.value = '';
  gotoInput.focus();
}
gotoForm.addEventListener('submit', (e) => {
  e.preventDefault();
  gotoForm.hidden = true;
  if (gotoMode === 'merge' && gotoInput.value.trim() !== '') {
    send({ type: 'c_merge', path: gotoInput.value.trim() });
  } else if (gotoMode === 'projection' && active) {
    // projection Presentation (§4.4): Print Records and parsing Rules - Key Module
    send({
      type: 'c_projection',
      view: active.data.view,
      key: gotoInput.value.trim(),
    });
  } else if (active && gotoInput.value.trim() !== '') {
    send({ type: 'c_goto', view: active.data.view, text: gotoInput.value });
  }
  activate(active);
});
gotoInput.addEventListener('keydown', (e) => {
  if (e.key !== 'Escape') return;
  gotoForm.hidden = true;
  activate(active);
});

// Guidance: Node under the pointer after pause - VM responds with a hint (type, value,
// error, ads with links). The hint is held as long as the pointer is above it or above
// the same node.
let hoverAt: { x: number; y: number } | undefined;
let hoverTimer: ReturnType<typeof setTimeout> | undefined;
let hoverNode = -1;
// Keys - VM (except for developer tools and Ctrl+P - signature strings).
for (const pane of [app, side]) {
  pane.addEventListener('keydown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || (e.ctrlKey && e.shiftKey && e.key === 'I')) return;
    if (['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
    v.hideHover();
    if (e.isComposing || e.key === 'Process') return; // IME: Text by compositionen d d
    if (e.key === 'F2') {
      e.preventDefault();
      byId('ast').hidden = !byId('ast').hidden;
      requestInspect();
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'A' || e.key === 'Ф')) {
      e.preventDefault();
      panes.classList.toggle('structure');
      if (!structure()) {
        inspector.showLabel('', 0, 0);
        statusLine.textContent = 'рамки узлов выключены';
      }
      requestInspect();
      return;
    }
    // Ctrl+V - Insert: paste event carries buffer text (buffer - by window)
    if (e.ctrlKey && !e.shiftKey && (e.key === 'v' || e.key === 'м')) return;
    e.preventDefault();
    if (e.ctrlKey && e.key === 'p') {
      openGoto();
      return;
    }
    if (e.key === 'F5') {
      run();
      return;
    }
    if (e.key === 'F6') {
      trace();
      return;
    }
    if (e.ctrlKey && e.key === 'm') {
      openGoto('merge');
      return;
    }
    if (e.ctrlKey && e.key === 'j') {
      openGoto('projection');
      return;
    }
    if (e.ctrlKey && e.key === 's') {
      send({ type: 'c_save' });
      return;
    }
    // Escape in the second panel without an open list - close the panel
    if (e.key === 'Escape' && pane === side && !v.listOpen && !debugging) {
      closeSide();
      return;
    }
    const mods =
      (e.shiftKey ? Mods.shift : 0) |
      (e.ctrlKey ? Mods.ctrl : 0) |
      (e.altKey ? Mods.alt : 0) |
      (e.metaKey ? Mods.meta : 0);
    send({ type: 'c_key', view: v.data.view, key: e.key, mods });
  });
  pane.addEventListener('paste', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    const text = e.clipboardData?.getData('text/plain') ?? '';
    e.preventDefault();
    if (v && text !== '') send({ type: 'c_input', view: v.data.view, text });
  });
  pane.addEventListener('compositionend', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (v && e.data !== '')
      send({ type: 'c_input', view: v.data.view, text: e.data });
  });
  // click cursor; Shift + click and pull - highlight; double - word, triple - string
  // (interprets VM)
  let dragging = false;
  let last = '';
  pane.addEventListener('mousedown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || e.button !== 0) return;
    e.preventDefault();
    activate(v);
    const at = v.placeAt(e.clientX, e.clientY);
    if (!at) return;
    dragging = true;
    last = `${String(at.node)}/${String(at.piece)}/${String(at.offset)}`;
    if (e.detail >= 2 || e.shiftKey)
      send({
        type: 'c_select',
        view: v.data.view,
        ...at,
        clicks: e.shiftKey ? 1 : e.detail,
      });
    else send({ type: 'c_pointer', view: v.data.view, ...at });
  });
  pane.addEventListener('mousemove', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || !dragging || (e.buttons & 1) === 0) return;
    const at = v.placeAt(e.clientX, e.clientY);
    if (!at) return;
    const key = `${String(at.node)}/${String(at.piece)}/${String(at.offset)}`;
    if (key === last) return;
    last = key;
    send({ type: 'c_select', view: v.data.view, ...at, clicks: 1 });
  });
  pane.addEventListener('mouseup', () => {
    dragging = false;
  });
  pane.addEventListener('mousemove', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || dragging) return;
    if (e.composedPath().includes(v.tip)) {
      clearTimeout(hoverTimer);
      return;
    }
    const n = ViewElement.nodeOf(e);
    if (n?.nodeId !== hoverNode) v.hideHover();
    clearTimeout(hoverTimer);
    if (!n) {
      hoverNode = -1;
      return;
    }
    hoverTimer = setTimeout(() => {
      hoverNode = n.nodeId;
      hoverAt = { x: e.clientX, y: e.clientY };
      send({ type: 'c_hover', view: v.data.view, node: n.nodeId });
    }, 400);
  });
  // Structure mode: view and place of node under the pointer, path in the state bar
  pane.addEventListener('mousemove', (e) => {
    if (!structure()) return;
    const path = e
      .composedPath()
      .filter(
        (x): x is Element =>
          x instanceof Element && x.classList.contains('tz-node'),
      )
      .reverse();
    const inner = path.at(-1);
    statusLine.textContent = inspector.describe(path);
    const r = inner
      ? inspector.roles.get(Number(inner.getAttribute('data-id')))
      : undefined;
    inspector.showLabel(
      inner
        ? [inspector.describe([inner]), r?.rule ?? '']
            .filter((x) => x !== '')
            .join('  ← ')
        : '',
      e.clientX,
      e.clientY,
    );
  });
  pane.addEventListener('mouseleave', () => {
    inspector.showLabel('', 0, 0);
    clearTimeout(hoverTimer);
    hoverNode = -1;
    for (const v of views.values()) if (v.host === pane) v.hideHover();
  });
}
