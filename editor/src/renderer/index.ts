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
  type Output,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';
import { ViewElement } from './view';
import { Inspector } from './inspector';
import { defineNodes } from './nodes/index';
import {
  resolve,
  type Host,
  type OutRow,
  type StatusView,
  type Theme,
} from './theme';

declare global {
  interface Window {
    vm: VmBridge;
  }
}

// Topic (theme.ts): The themes/<name>/ folders are picked up by themselves, the selected one is transmitted by
// the main process (-theme <name> → ?theme=). Styles, window markings and node elements - to everything else.
const themes = new Map(
  Object.values(
    import.meta.glob<{ default: Theme }>('./themes/*/index.ts', {
      eager: true,
    }),
  ).map((m) => [m.default.name, m.default]),
);
const theme = resolve(
  themes,
  new URLSearchParams(location.search).get('theme') ?? 'canvas',
);
for (const css of theme.css)
  document.head.append(
    Object.assign(document.createElement('style'), { textContent: css }),
  );
document.body.innerHTML = theme.shell;
document.body.dataset.theme = theme.name;
defineNodes(theme.nodes);
const panels = theme.panels;

function byId(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`нет элемента #${id}`);
  return el;
}

const app = byId('app');
const side = byId('side');
const sideWrap = byId('side-wrap');
const layersBar = byId('layers');
const header = document.getElementById('header'); // The hat may not be (classic)
const params = new URLSearchParams(location.search);
const gotoForm = byId('goto') as HTMLFormElement;
const gotoInput = byId('goto-input') as HTMLInputElement;
const mergePanel = byId('merge');
const proposalsPanel = byId('proposals');
const aiPanel = byId('ai');
const aiLog = aiPanel.querySelector<HTMLElement>('.log') ?? aiPanel;
const aiForm = byId('ai-form') as HTMLFormElement;
const aiInput = byId('ai-input') as HTMLTextAreaElement;
type GotoMode = 'goto' | 'merge' | 'projection' | 'apply';
let gotoMode: GotoMode = 'goto';
const statusLine = byId('status');
const runPanel = byId('run');
const runStdin = byId('run-stdin') as HTMLTextAreaElement;
const runArgs = byId('run-args') as HTMLInputElement;
const runOutput = byId('run-output');
const stopsPanel = byId('stops');
const views = new Map<number, ViewElement>();
let active: ViewElement | undefined;
let sideModule = false; // We wait for the second presentation of the module - in the second panel
let debugging = false; // Open track: Escape - VM (exit)

function send(m: ClientMsg): void {
  window.vm.send(encodeClient(m));
}
const mainView = () => [...views.values()].find((x) => x.host === app);
// What theme panels can do (theme.ts)
const host: Host = {
  send,
  open(node) {
    const v = mainView();
    if (v) send({ type: 'c_open', view: v.data.view, node });
  },
  reconnect() {
    window.vm.reconnect();
  },
  goto(text) {
    if (active) send({ type: 'c_goto', view: active.data.view, text });
  },
};

// Inspector AST (F2 - wood panel, Ctrl+Shift+A - frame node in the text): the element tree
// under the cursor, places node and by whom they are disassembled - from VM.
const panes = byId('panes');
// ‹i/n› function with overload - open it in the second panel (there is a switch)
panes.addEventListener('vish-open', (e) => {
  const v = [...views.values()].find((x) => x.host.contains(e.target as Node));
  if (v && e instanceof CustomEvent)
    send({ type: 'c_open', view: v.data.view, node: e.detail as NodeId });
});
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

// Card entity on the cursor (frame "cursor on stream", author's decision 2026-10-27): the cursor
// stood on the function name - the window asks for a hint of this node; the card collects VM.
let cardTimer: ReturnType<typeof setTimeout> | undefined;
const cardRoles = ['fn', 'prelude', 'native', 'fiber'];
function cursorCard(v: ViewElement | undefined, node: NodeId | null): void {
  clearTimeout(cardTimer);
  const el = node === null ? undefined : v?.byId.get(node);
  if (!v || node === null || !cardRoles.includes(el?.dataset.role ?? ''))
    return;
  cardTimer = setTimeout(() => {
    hoverAt = undefined;
    send({ type: 'c_hover', view: v.data.view, node });
  }, 500);
}

function activate(v: ViewElement | undefined): void {
  active = v;
  (v?.host ?? app).focus();
  paintHeader();
}

// Header and line numbers (draws the theme): the program file, the result of checking the module, the position
// of the cursor of the active panel. Checked - the marks came to the last edit; errors - error marks.
const checks = new Map<number, { revision: number; errors: number }>();
function paintHeader(): void {
  if (!header) return;
  const main = mainView();
  const at = active?.cursor
    ? active.position(active.cursor)
    : { line: 1, col: 1 };
  const c = main ? checks.get(main.data.view) : undefined;
  panels.header(
    header,
    {
      file: params.get('file') ?? '',
      errors: c?.errors ?? 0,
      checked: c !== undefined && c.revision >= 0,
      line: at.line,
      col: at.col,
    },
    host,
  );
}
function paintGutter(v: ViewElement | undefined): void {
  if (v) panels.gutter(v.gutter, v.lines());
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
      showStatus({ kind: '', text: '', hint: '' });
      send({ type: 'c_open_view', kind: 'module' });
      return;
    case 's_view': {
      // second view of the module (Ctrl+Shift+J) - in the second panel: the same program in another projection
      const pane =
        views.get(m.view)?.host ?? (m.kind === 'fn' || sideModule ? side : app); // famous
      const fresh = !views.has(m.view);
      if (m.kind === 'module') sideModule = false;
      sideWrap.hidden = sideWrap.hidden && pane !== side;
      const v = new ViewElement(pane, m);
      v.paintTip = (el, lines, open) => {
        panels.tip(el, lines, open, host);
      };
      v.paintComplete = panels.complete;
      views.set(m.view, v);
      paintGutter(v);
      activate(v);
      if (fresh && pane === side && m.kind === 'module') openGoto('projection'); // her projection is the key
      return;
    }
    case 's_update':
      views.get(m.view)?.update(m);
      paintGutter(views.get(m.view));
      checks.set(m.view, {
        revision: -1,
        errors: checks.get(m.view)?.errors ?? 0,
      }); // tested
      paintHeader();
      requestInspect();
      return;
    case 's_cursor':
      views.get(m.view)?.showCursor(m);
      if (m.node !== null) cursorNode.set(m.view, m.node);
      paintHeader();
      cursorCard(views.get(m.view), m.node);
      requestInspect();
      return;
    case 's_inspect': {
      const v = views.get(m.view);
      if (v) inspector.show(v, m);
      return;
    }
    case 's_decor':
      views.get(m.view)?.showMarks(m);
      paintGutter(views.get(m.view)); // commentaries
      checks.set(m.view, {
        revision: m.revision,
        errors: m.marks.filter((k) => k.kind === 'error').length,
      });
      paintHeader();
      return;
    case 's_layers':
      panels.layers(layersBar, m, host);
      return;
    case 's_complete':
      views.get(m.view)?.showCompletion(m);
      return;
    case 's_output': {
      showOutput(m);
      return;
    }
    case 's_trace':
      debugging = m.active;
      stopsPanel.hidden = !m.active;
      if (!m.active) return;
      runPanel.hidden = false;
      runOutput.textContent = m.output + (m.error === '' ? '' : `\n${m.error}`);
      runOutput.toggleAttribute('error', m.error !== '');
      return;
    case 's_proposals':
      panels.proposals(proposalsPanel, m.proposals, host);
      return;
    case 's_items':
      return;
    case 's_merge':
      panels.merge(mergePanel, m.conflicts, host);
      return;
    case 's_saved': // VM sends a bang (s_status)
      return;
    case 's_stops':
      panels.stops(stopsPanel, m, host);
      return;
    case 's_peers': {
      const v = views.get(m.view);
      v?.showPeers(m);
      if (v?.host === app) panels.clients(byId('clients'), m, host);
      return;
    }
    case 's_status':
      showStatus(m);
      return;
    case 's_error':
      flashStatus('error', m.text);
      return;
    case 's_select':
      views.get(m.view)?.showSelection(m);
      return;
    case 's_hover': {
      const v = views.get(m.view);
      // cursor (no hoverAt) - only entity card
      const card = m.lines.some((l) => l.kind === 'имя');
      if (v && (hoverAt || card))
        v.showHover(m, hoverAt?.x ?? 0, hoverAt?.y ?? 0, (node) => {
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
        flashStatus('error', `буфер обмена: ${String(e)}`);
      });
      return;
  }
}

// AI Panel: query to your Claude Code through the main process; in journal, answer, vish tool
// calls, and end. Changes come in sentences (panel below), accepted by the person.
//
function aiLine(kind: string, text: string): void {
  const row = document.createElement('div');
  row.className = `ai-${kind}`;
  row.textContent = text;
  aiLog.append(row);
  aiLog.scrollTop = aiLog.scrollHeight;
}
let aiBusy = false;
aiForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = aiInput.value.trim();
  if (text === '' || aiBusy) return;
  aiBusy = true;
  aiForm.toggleAttribute('busy', true);
  aiLine('user', text);
  aiInput.value = '';
  window.vm.aiAsk(text);
});
aiInput.addEventListener('keydown', (e) => {
  e.stopPropagation(); // field keys - not to the editor
  if (e.key === 'Enter' && e.ctrlKey) aiForm.requestSubmit();
  if (e.key === 'Escape' && aiBusy) window.vm.aiStop();
});
window.vm.onAi((kind, text) => {
  if (kind === 'done' || kind === 'error') {
    aiBusy = false;
    aiForm.toggleAttribute('busy', false);
  }
  if (kind !== 'done' || text !== '') aiLine(kind, text);
});

// Launch Output (Run frame): Lines with the signature of their print accumulate while VM sends more;
// executable - backlight in the code.
let outRows: OutRow[] = [];
function showOutput(m: Output): void {
  if (!m.more) outRows = [];
  const lines = m.text.split('\n');
  if (lines.at(-1) === '') lines.pop();
  lines.forEach((text, i) => {
    const src = m.from.at(i);
    outRows.push({ text, label: src?.label ?? '', node: src?.node ?? null });
  });
  runOutput.toggleAttribute('error', !m.ok);
  panels.output(runOutput, outRows, host);
  mainView()?.showRunning(m.now);
}

// Status bang: the state from VM (error, track, launch; "" - to hide) holds, the event ("done", VM message)
// - a few seconds on top of it. After the event, VM sends peace. Brush button (frame "Office states"):
// "goto signature" - the transition list (there "add"), "reconnect" - connect to the VM again
// (communication - the main process).
let steady: StatusView = { kind: '', text: '', hint: '' };
let flashTimer: ReturnType<typeof setTimeout> | undefined;
function paintStatus(s: StatusView): void {
  panels.status(statusLine, s, host);
}
function flashStatus(kind: string, text: string, hint = ''): void {
  clearTimeout(flashTimer);
  paintStatus({ kind, text, hint });
  flashTimer = setTimeout(() => {
    flashTimer = undefined;
    paintStatus(steady);
  }, 3000);
}
function showStatus(s: StatusView): void {
  if (s.kind === 'done') {
    steady = { kind: '', text: '', hint: '' };
    flashStatus(s.kind, s.text, s.hint);
    return;
  }
  steady = { ...s };
  if (flashTimer === undefined) paintStatus(steady);
}

const split = new FrameSplitter();
window.vm.onData((bytes) => {
  split.push(bytes, (body) => {
    receive(decodeServer(body));
  });
});
window.vm.onStatus((text, lost) => {
  // communication with VM - to s_hello; disconnected - “Connect” button
  showStatus(
    lost
      ? {
          kind: 'error',
          text,
          hint: '',
          action: 'reconnect',
          label: 'Подключить',
        }
      : { kind: 'vm', text, hint: '' },
  );
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
  runOutput.dataset.text = '';
  send({ type: 'c_run', stdin_text: runStdin.value, args: runArgs.value });
}
// Shift+F5 - Stop Launch: Stop/Run
function stop(): void {
  send({ type: 'c_stop' });
}
for (const field of [runStdin, runArgs] as HTMLElement[]) {
  field.addEventListener('keydown', (e) => {
    if (e.key === 'F5') {
      e.preventDefault();
      if (e.shiftKey) stop();
      else run();
    }
    if (e.key === 'Escape') activate(active);
  });
}

// Ctrl+P is the signature string; it is sent by Enter, Escape is closed.
function openGoto(mode: GotoMode = 'goto'): void {
  gotoMode = mode;
  gotoInput.placeholder =
    mode === 'goto'
      ? 'перейти или добавить: foo(i64)'
      : mode === 'merge'
        ? 'слить с базой: путь.vishdb'
        : mode === 'apply'
          ? 'применить функцию правки программы: имя (code_rename, code_replace…)'
          : 'проекция: ключ модуля записей (пусто — по умолчанию)';
  gotoForm.hidden = false;
  gotoInput.value = '';
  gotoInput.focus();
}
gotoForm.addEventListener('submit', (e) => {
  e.preventDefault();
  gotoForm.hidden = true;
  if (gotoMode === 'apply' && gotoInput.value.trim() !== '') {
    runPanel.hidden = false;
    runOutput.textContent = '…';
    send({ type: 'c_apply', fn: gotoInput.value.trim() });
  } else if (gotoMode === 'merge' && gotoInput.value.trim() !== '') {
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
// Keys - VM (except for developer tools and Ctrl+P - signature strings). Two projections are next: the
// node under the pointer - and in another panel (the same node of the tree, id common).
let twin: Element | undefined;
function showTwin(from: HTMLElement, id: string | null): void {
  twin?.removeAttribute('twin');
  twin = undefined;
  const other = from === app ? side : app;
  if (id === null || sideWrap.hidden) return;
  twin = other.querySelector(`[data-id="${id}"]`) ?? undefined;
  twin?.setAttribute('twin', '');
}
for (const pane of [app, side]) {
  pane.addEventListener('mousemove', (e) => {
    showTwin(pane, ViewElement.nodeOf(e)?.getAttribute('data-id') ?? null);
  });
  pane.addEventListener('keydown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || (e.ctrlKey && e.shiftKey && e.key === 'I')) return;
    if (['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
    v.hideHover();
    if (e.isComposing || e.key === 'Process') return; // IME: Text by compositionen d d
    if (e.key === 'F2') {
      e.preventDefault();
      byId('ast').hidden = !byId('ast').hidden;
      if (byId('ast').hidden)
        for (const w of views.values()) w.showGhost(null, '');
      requestInspect();
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'R' || e.key === 'К')) {
      e.preventDefault();
      openGoto('apply');
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'A' || e.key === 'Ф')) {
      e.preventDefault();
      panes.classList.toggle('structure');
      if (!structure()) {
        inspector.showLabel('', 0, 0);
        flashStatus('done', 'рамки узлов выключены');
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
      if (e.shiftKey) stop();
      else run();
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
    // Ctrl+Shift+J - two projections side by side (frame “Projection”): module again in the second panel
    if (e.ctrlKey && e.shiftKey && (e.key === 'J' || e.key === 'О')) {
      e.preventDefault();
      sideModule = true;
      send({ type: 'c_open_view', kind: 'module' });
      return;
    }
    // Ctrl+I - “AI” panel (your Claude Code, edits - sentences)
    if (e.ctrlKey && (e.key === 'i' || e.key === 'ш')) {
      e.preventDefault();
      aiPanel.hidden = !aiPanel.hidden;
      if (!aiPanel.hidden) aiInput.focus();
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
    v.pointPath(n);
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
          x instanceof Element && x.classList.contains('vish-node'),
      )
      .reverse();
    const inner = path.at(-1);
    flashStatus('path', inspector.describe(path)); // path node under pointer
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
