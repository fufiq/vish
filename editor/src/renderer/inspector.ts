// AST Inspector (§13.1): what is and who is disassembled. The tree of the module element under
// the cursor is from VM (s_inspect): the place in the parent (field node and the syntax or
// construction recording parameter), the view, its text, types, by whom it is disassembled; at
// the bottom - the node disclosure at the cursor (record: piece → parameter, result;
// construction: a body with substituted arguments). Structure mode: frame node in the text,
// pointer - the view and place node, path - in the status bar.
//
//
import type { Inspect, InspectNode, NodeId } from '../protocol/messages';
import { renderLines, type ViewElement } from './view';

export class Inspector {
  readonly panel: HTMLElement;
  readonly tree = document.createElement('div');
  readonly expand = document.createElement('div');
  readonly label = document.createElement('div');
  // node place in the parent - according to the last VM response (for structure mode)
  readonly roles = new Map<NodeId, InspectNode>();
  #view: ViewElement | undefined;

  constructor(
    panel: HTMLElement,
    readonly actions: {
      open(view: ViewElement, node: NodeId): void; // citation
      place(view: ViewElement, node: NodeId): void; // node
    },
  ) {
    this.panel = panel;
    const head = document.createElement('div');
    head.className = 'head';
    head.textContent = 'AST — F2 закрыть, Ctrl+Shift+A — рамки в тексте';
    this.tree.className = 'tree';
    this.expand.className = 'expand';
    panel.replaceChildren(head, this.tree, this.expand);
    this.label.className = 'tz-structure-label';
    this.label.hidden = true;
    document.body.append(this.label);
  }

  get open(): boolean {
    return !this.panel.hidden;
  }

  show(view: ViewElement, m: Inspect): void {
    this.#view = view;
    this.roles.clear();
    for (const n of m.nodes) this.roles.set(n.id, n);
    for (const [id, el] of view.byId) {
      const r = this.roles.get(id);
      if (r) el.dataset.role = r.role;
    }
    // focus and its ancestors - highlighted
    const path = new Set<NodeId>();
    const stack: InspectNode[] = [];
    for (const n of m.nodes) {
      stack.length = n.depth;
      stack.push(n);
      if (n.id === m.focus) for (const x of stack) path.add(x.id);
    }
    this.tree.replaceChildren(
      ...m.nodes.map((n) => {
        const row = document.createElement('div');
        row.className = 'row';
        row.style.paddingLeft = `${String(8 + n.depth * 14)}px`;
        row.toggleAttribute('path', path.has(n.id));
        row.toggleAttribute('focus', n.id === m.focus);
        const add = (cls: string, text: string) => {
          if (text === '') return;
          const s = document.createElement('span');
          s.className = cls;
          s.textContent = text;
          row.append(s);
        };
        add('role', n.role);
        add('kind', n.kind);
        add('text', n.text);
        add('types', n.types === '' ? '' : `: ${n.types}`);
        if (n.rule !== '') {
          const rule = document.createElement('span');
          rule.className = 'rule';
          rule.textContent = `← ${n.rule}`;
          const at = n.rule_at;
          if (at !== null)
            rule.addEventListener('mousedown', (e) => {
              e.preventDefault();
              e.stopPropagation();
              this.actions.open(view, at);
            });
          row.append(rule);
        }
        row.addEventListener('mouseenter', () => {
          this.#mark(n.id);
        });
        row.addEventListener('mouseleave', () => {
          this.#mark(null);
        });
        row.addEventListener('mousedown', (e) => {
          e.preventDefault();
          this.actions.place(view, n.id);
        });
        return row;
      }),
    );
    this.tree.querySelector('[focus]')?.scrollIntoView({ block: 'nearest' });
    this.expand.hidden = m.expand.length === 0;
    renderLines(this.expand, m.expand, (node) => {
      this.actions.place(view, node);
    });
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
        const role = el.getAttribute('data-role') ?? '';
        return role === '' ? kind : `${role}: ${kind}`;
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
