// AST Inspector (§13.1): what is and who is disassembled. The tree of the module element under the
// cursor is from VM (s_inspect): the place in the parent (field node and the syntax or construction
// recording parameter), the view, its text, types, by whom it is disassembled; the disclosure of node
// at the cursor (record: piece → parameter, result; construction: a body with substituted arguments).
// 
// . Draw a tree and open a theme bar (inspect, expansion); disclosure - a block under node in the code
// or a ghost string with full disclosure in the panel (author’s decision 2026-10-27: both, switch).
// Structure mode: frame node in the text, pointer - the view and place node, path - in the status bar.
//
//
import type { Inspect, InspectNode, NodeId } from '../protocol/messages';
import type { InspectActions, Panels } from './theme';
import type { ViewElement } from './view';

export type ExpandMode = 'block' | 'line';

// Whose parsing is it: the focus or closest ancestor with the rule (the cursor on the design name is its challenge)
export function owner(m: Inspect): NodeId | null {
  const at = m.nodes.findIndex((n) => n.id === m.focus);
  let depth = m.nodes.at(at)?.depth ?? 0;
  for (let k = at; k >= 0; --k) {
    const n = m.nodes.at(k);
    if (!n || n.depth > depth) continue;
    depth = n.depth;
    if (n.rule !== '') return n.id;
  }
  return m.focus;
}

export class Inspector {
  readonly panel: HTMLElement;
  readonly tree = document.createElement('div');
  readonly expand = document.createElement('div');
  readonly label = document.createElement('div');
  readonly modes = document.createElement('div');
  // node place in the parent - according to the last VM response (for structure mode)
  readonly roles = new Map<NodeId, InspectNode>();
  mode: ExpandMode = 'block';
  #view: ViewElement | undefined;
  #last: Inspect | undefined;

  constructor(
    panel: HTMLElement,
    readonly actions: {
      open(view: ViewElement, node: NodeId): void; // citation
      place(view: ViewElement, node: NodeId): void; // node
      hover(view: ViewElement, node: NodeId | null, x: number, y: number): void; // node card
      menu(view: ViewElement, node: NodeId, x: number, y: number): void; // Editing panel
    },
    readonly paint: Pick<Panels, 'inspect' | 'expansion'>,
  ) {
    this.panel = panel;
    const head = document.createElement('div');
    head.className = 'head';
    const title = document.createElement('span');
    title.className = 'title';
    title.textContent = 'AST — F2 закрыть, Ctrl+Shift+A — рамки в тексте';
    this.modes.className = 'modes';
    for (const [mode, text] of [
      ['block', 'раскрытие блоком'],
      ['line', 'строкой'],
    ] as const) {
      const b = document.createElement('button');
      b.dataset.mode = mode;
      b.textContent = text;
      b.addEventListener('mousedown', (e) => {
        e.preventDefault();
        this.mode = mode;
        this.#repaint();
      });
      this.modes.append(b);
    }
    head.append(title, this.modes);
    this.tree.className = 'tree';
    this.expand.className = 'expand';
    panel.replaceChildren(head, this.tree, this.expand);
    this.label.className = 'vish-structure-label';
    this.label.hidden = true;
    document.body.append(this.label);
  }

  get open(): boolean {
    return !this.panel.hidden;
  }

  #acts(view: ViewElement): InspectActions {
    return {
      open: (node) => {
        this.actions.open(view, node);
      },
      place: (node) => {
        this.actions.place(view, node);
      },
      mark: (node) => {
        this.#mark(node);
      },
      hover: (node, x, y) => {
        this.actions.hover(view, node, x, y);
      },
      menu: (node, x, y) => {
        this.actions.menu(view, node, x, y);
      },
    };
  }

  show(view: ViewElement, m: Inspect): void {
    this.#view = view;
    this.#last = m;
    this.roles.clear();
    for (const n of m.nodes) this.roles.set(n.id, n);
    // node place in the parent - data-place (data-role - the role of the name for color, paragraph 2)
    for (const [id, el] of view.byId) {
      const r = this.roles.get(id);
      if (r) el.dataset.place = r.role;
    }
    this.#repaint();
  }

  #repaint(): void {
    const view = this.#view;
    const m = this.#last;
    for (const b of this.modes.querySelectorAll('button'))
      b.toggleAttribute('selected', b.dataset.mode === this.mode);
    if (!view || !m) return;
    const acts = this.#acts(view);
    this.paint.inspect(this.tree, this.expand, m, acts);
    this.tree.querySelector('[focus]')?.scrollIntoView({ block: 'nearest' });
    const line = this.mode === 'line';
    this.expand.hidden = !line || m.expand.length === 0;
    // line: what the focus unfolded - a ghost in one line; block - under node in the code
    const into = m.expand.find(
      (l) => l.kind === 'раскрытие' || l.kind === 'итог',
    );
    view.showGhost(
      m.focus,
      line && into ? into.text.replace(/\s*\n\s*/g, ' ') : '',
    );
    view.showBlock(line || m.expand.length === 0 ? null : owner(m), (el) => {
      this.paint.expansion(el, m, acts);
    });
  }

  hide(): void {
    this.#view?.showGhost(null, '');
    this.#view?.showBlock(null, () => undefined);
  }

  // The node in the text is illuminated (hinting to a tree line).
  #marked: Element | undefined;
  #mark(id: NodeId | null): void {
    this.#marked?.removeAttribute('inspected');
    this.#marked = id === null ? undefined : this.#view?.byId.get(id);
    this.#marked?.setAttribute('inspected', '');
  }

  // Structure mode: the pointer has the view and place node; the path has the line.
  describe(path: Element[]): string {
    return path
      .map((el) => {
        const kind = el.getAttribute('data-kind') ?? '';
        const place = el.getAttribute('data-place') ?? '';
        return place === '' ? kind : `${place}: ${kind}`;
      })
      .join(' › ');
  }
  showLabel(text: string, x: number, y: number): void {
    this.label.hidden = text === '';
    this.label.textContent = text;
    this.label.style.left = `${String(x + 12)}px`;
    this.label.style.top = `${String(y + 16)}px`;
  }
}
