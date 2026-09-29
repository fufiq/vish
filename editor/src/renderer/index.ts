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
  type ServerMsg,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';
import { ViewElement } from './view';
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
const gotoForm = byId('goto') as HTMLFormElement;
const gotoInput = byId('goto-input') as HTMLInputElement;
const statusLine = byId('status');
const views = new Map<number, ViewElement>();
let active: ViewElement | undefined;

function send(m: ClientMsg): void {
  window.vm.send(encodeClient(m));
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
  side.hidden = true;
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
      side.hidden = side.hidden && host !== side;
      const v = new ViewElement(host, m);
      views.set(m.view, v);
      activate(v);
      return;
    }
    case 's_update':
      views.get(m.view)?.update(m);
      statusLine.textContent = `ревизия ${String(m.revision)}`;
      return;
    case 's_cursor':
      views.get(m.view)?.showCursor(m);
      return;
    case 's_decor': {
      const error = views.get(m.view)?.showMarks(m);
      if (views.get(m.view) !== active) return;
      statusLine.textContent =
        error === undefined
          ? `ревизия ${String(m.revision)}`
          : `ошибка: ${error.split('\n')[0] ?? ''}`;
      statusLine.toggleAttribute('error', error !== undefined);
      return;
    }
    case 's_complete':
      views.get(m.view)?.showCompletion(m);
      return;
    case 's_error':
      statusLine.textContent = `VM: ${m.text}`;
      return;
  }
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

// Ctrl+P is the signature string; it is sent by Enter, Escape is closed.
function openGoto(): void {
  gotoForm.hidden = false;
  gotoInput.value = '';
  gotoInput.focus();
}
gotoForm.addEventListener('submit', (e) => {
  e.preventDefault();
  gotoForm.hidden = true;
  if (active && gotoInput.value.trim() !== '') {
    send({ type: 'c_goto', view: active.data.view, text: gotoInput.value });
  }
  activate(active);
});
gotoInput.addEventListener('keydown', (e) => {
  if (e.key !== 'Escape') return;
  gotoForm.hidden = true;
  activate(active);
});

// Keys - VM (except for developer tools and Ctrl+P - signature strings).
for (const pane of [app, side]) {
  pane.addEventListener('keydown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || (e.ctrlKey && e.shiftKey && e.key === 'I')) return;
    if (['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
    e.preventDefault();
    if (e.ctrlKey && e.key === 'p') {
      openGoto();
      return;
    }
    // Escape in the second panel without an open list - close the panel
    if (e.key === 'Escape' && pane === side && !v.listOpen) {
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
  pane.addEventListener('mousedown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v) return;
    e.preventDefault();
    activate(v);
    const at = v.placeAt(e.clientX, e.clientY);
    if (at) send({ type: 'c_pointer', view: v.data.view, ...at });
  });
}
