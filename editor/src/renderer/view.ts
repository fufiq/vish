import type {
  Complete,
  Decor,
  Cursor,
  Hint,
  HintLine,
  Hover,
  NodeId,
  Peers,
  Selection,
  Update,
  View,
  ViewNode,
} from '../protocol/messages';
import {
  ASTNode,
  codePoints,
  pieceOfText,
  startOfText,
  utf16Offset,
  type NodeMarks,
  type PieceText,
} from './nodes/ast_node';
import { RecordNode } from './nodes/record_node';
import { tagOf } from './nodes/index';

// Representation of the module in the window: node elements on the pieces sent, the
// cursor is where the VM holds it. The item is addressed directly to node_id:
// - there's no model at the window.
export class ViewElement {
  readonly byId = new Map<NodeId, ASTNode>();
  readonly caret = document.createElement('div');
  readonly popup = document.createElement('ul');
  // Selection - rectangles on top of the text (the text is also in Shadow DOM node)
  readonly selectionLayer = document.createElement('div');
  // Instructions: guidance (at the pointer) and set (above the cursor)
  readonly tip = document.createElement('div');
  readonly hint = document.createElement('div');
  // other clients: their carriages and frame elements in their draft (frame "Two Clients")
  readonly peersLayer = document.createElement('div');
  // “will be created” - next to the transition list when you select “add” (frame “Transition”)
  readonly preview = document.createElement('div');
  // revealing the inspector's node with a ghost string under it (frame "Simple Disclosure")
  readonly ghost = document.createElement('div');
  // Line number - draws the theme (panels.gutter), there is only a place in the window
  readonly gutter = document.createElement('div');
  root: ASTNode | undefined;
  // Draws items from the supplement list (theme panel complete)
  paintComplete: (el: HTMLElement, m: Complete) => void = renderComplete;
  // Draws a hint and a card (theme panel tip)
  paintTip: (
    el: HTMLElement,
    lines: readonly HintLine[],
    open: (node: NodeId) => void,
  ) => void = renderLines;

  readonly nodes = new Map<NodeId, ViewNode>();

  constructor(
    readonly host: HTMLElement,
    readonly data: View,
  ) {
    for (const n of data.nodes) this.nodes.set(n.id, n);
    this.root = this.#build(data.root);
    this.caret.className = 'vish-caret';
    this.popup.className = 'vish-complete';
    this.popup.hidden = true;
    this.selectionLayer.className = 'vish-selection';
    this.tip.className = 'vish-tip';
    this.tip.hidden = true;
    this.hint.className = 'vish-tip vish-hint';
    this.hint.hidden = true;
    this.peersLayer.className = 'vish-peers';
    this.preview.className = 'vish-preview';
    this.preview.hidden = true;
    this.ghost.className = 'vish-ghost';
    this.ghost.hidden = true;
    this.gutter.className = 'vish-gutter';
    host.replaceChildren(...this.#chrome(this.root));
  }

  #chrome(root: ASTNode): Node[] {
    return [
      this.gutter,
      this.selectionLayer,
      root,
      this.peersLayer,
      this.caret,
      this.popup,
      this.preview,
      this.ghost,
      this.hint,
      this.tip,
    ];
  }

  // Guidance - at the pointer (x, y - coordinates of the window).
  showHover(m: Hover, x: number, y: number, open: (n: NodeId) => void): void {
    this.tip.hidden = m.lines.length === 0;
    if (this.tip.hidden) return;
    this.paintTip(this.tip, m.lines, open);
    const origin = this.host.getBoundingClientRect();
    // entity card (frame “cursor on stream”) - above node; other - at the pointer
    const card = m.lines.some((l) => l.kind === 'имя' || l.kind === 'ошибка');
    this.tip.toggleAttribute('card', card);
    const el = m.node === null ? undefined : this.byId.get(m.node);
    if (card && el) {
      // above node, if there is more space (as in the frame), otherwise under it; not above free space
      const r = el.getBoundingClientRect();
      const left = Math.max(
        8,
        r.left - origin.left + this.host.scrollLeft - 24,
      );
      this.tip.style.left = `${String(left)}px`;
      const above = r.top - origin.top - 10;
      const below = origin.bottom - r.bottom - 10;
      this.tip.style.maxHeight = `${String(Math.max(above, below) - 8)}px`;
      const h = this.tip.offsetHeight;
      this.tip.style.top = `${String(
        above >= below
          ? r.top - origin.top + this.host.scrollTop - h - 10
          : r.bottom - origin.top + this.host.scrollTop + 10,
      )}px`;
      return;
    }
    this.tip.style.maxHeight = '';
    this.tip.style.left = `${String(x - origin.left + this.host.scrollLeft)}px`;
    this.tip.style.top = `${String(y - origin.top + this.host.scrollTop + 18)}px`;
  }
  hideHover(): void {
    this.tip.hidden = true;
  }

  // The tip of the set is above the place (the beginning of the word or call name).
  showHint(m: Hint, open: (n: NodeId) => void): void {
    const box = this.#box(m.node, m.piece, m.offset);
    this.hint.hidden = m.lines.length === 0 || !box;
    if (this.hint.hidden || !box) return;
    this.paintTip(this.hint, m.lines, open);
    this.hint.style.left = `${String(box.left)}px`;
    this.hint.style.top = `${String(box.top)}px`;
  }

  // AST node under the pointer (the deepest).
  static nodeOf(e: Event): ASTNode | undefined {
    return e.composedPath().find((x): x is ASTNode => x instanceof ASTNode);
  }

  #build(id: NodeId): ASTNode {
    const n = this.nodes.get(id);
    if (!n) throw new Error(`узел #${String(id)} не прислан`);
    const el = document.createElement(tagOf(n.kind));
    if (!(el instanceof ASTNode)) {
      throw new Error(`вид ${n.kind} не зарегистрирован`);
    }
    el.nodeId = id;
    el.dataset.kind = n.kind;
    el.dataset.id = String(id);
    el.note = n.note;
    this.byId.set(id, el);
    el.show(n.pieces, (c) => this.#build(c));
    return el;
  }

  // Line and column (from 1) places in the text of the presentation: chunks in order to (node, piece,
  // offset); without space - the number of lines of text.
  position(at?: { node: NodeId | null; piece: number; offset: number }): {
    line: number;
    col: number;
  } {
    let line = 1;
    let col = 1;
    const add = (text: string) => {
      for (const ch of text)
        if (ch === '\n') {
          ++line;
          col = 1;
        } else ++col;
    };
    const walk = (id: NodeId): boolean => {
      for (const [i, p] of (this.nodes.get(id)?.pieces ?? []).entries()) {
        if (typeof p !== 'string') {
          if (walk(p)) return true;
          continue;
        }
        if (at?.node === id && i === at.piece) {
          // eslint-disable-next-line @typescript-eslint/no-misused-spread
          add([...p].slice(0, at.offset).join(''));
          return true;
        }
        add(p);
      }
      return false;
    };
    walk(this.root?.nodeId ?? this.data.root);
    return { line, col };
  }

  // Edit from VM: the node subtree is reassembled and replaced by the old (the same
  // slot parent); the rest of the DOM is left unmoved.
  update(u: Update): void {
    for (const n of u.nodes) this.nodes.set(n.id, n);
    const old = this.byId.get(u.node);
    const fresh = this.#build(u.node);
    if (!old || old === this.root) {
      this.root = fresh;
      this.host.replaceChildren(...this.#chrome(fresh));
    } else {
      fresh.slot = old.slot;
      old.replaceWith(fresh);
    }
    // node, which saved id, - with the same labels until the next check (VM saves id
    // node when dialing)
    if (this.#decor) this.showMarks(this.#decor);
    if (this.#peers) this.showPeers(this.#peers);
  }

  #decor: Decor | undefined;
  #peers: Peers | undefined;
  #running: Element | undefined;

  // Ghost line under node (null - remove).
  showGhost(node: NodeId | null, text: string): void {
    const el = node === null ? undefined : this.byId.get(node);
    this.ghost.hidden = !el || text === '';
    if (!el || this.ghost.hidden) return;
    const origin = this.host.getBoundingClientRect();
    const r = el.getBoundingClientRect();
    this.ghost.textContent = `⤷ ${text}`;
    this.ghost.style.left = `${String(r.left - origin.left + this.host.scrollLeft)}px`;
    this.ghost.style.top = `${String(r.bottom - origin.top + this.host.scrollTop)}px`;
  }

  // Executed by start (frame "Run"): node - backlit string; null - nothing.
  showRunning(node: NodeId | null): void {
    this.#running?.removeAttribute('running');
    this.#running = node === null ? undefined : this.byId.get(node);
    this.#running?.setAttribute('running', '');
  }

  // Other clients: a carriage with the signature "client N" and a dotted frame around the element he rules
  // with the draft; node is not in the layout - the nearest ancestor (VM has already substituted).
  showPeers(m: Peers): void {
    this.#peers = m;
    const out: HTMLElement[] = [];
    const origin = this.host.getBoundingClientRect();
    const at = (r: DOMRect) => ({
      left: r.left - origin.left + this.host.scrollLeft,
      top: r.top - origin.top + this.host.scrollTop,
      height: r.height,
      width: r.width,
    });
    for (const p of m.peers) {
      const name = p.name === '' ? `клиент ${String(p.client + 1)}` : p.name;
      if (p.draft_node !== null) {
        const el = this.byId.get(p.draft_node);
        if (el) {
          const r = at(el.getBoundingClientRect());
          const box = document.createElement('div');
          box.className = 'peer-draft';
          box.style.left = `${String(r.left - 6)}px`;
          box.style.top = `${String(r.top - 4)}px`;
          box.style.width = `${String(r.width + 12)}px`;
          box.style.height = `${String(r.height + 8)}px`;
          const label = document.createElement('span');
          label.textContent = `правит ${name} · черновик`;
          box.append(label);
          out.push(box);
        }
      }
      const node = p.node === null ? undefined : this.byId.get(p.node);
      const b =
        this.#box(p.node, p.piece, p.offset) ??
        (node ? at(node.getBoundingClientRect()) : undefined);
      if (!b) continue;
      const caret = document.createElement('div');
      caret.className = 'peer-caret';
      caret.style.left = `${String(b.left)}px`;
      caret.style.top = `${String(b.top)}px`;
      caret.style.height = `${String(b.height)}px`;
      const label = document.createElement('span');
      label.textContent = name;
      caret.append(label);
      out.push(caret);
    }
    this.peersLayer.replaceChildren(...out);
  }
  #selection: Selection | undefined;

  // Texts of pieces in order projection : (knot, piece, beginning in piece) → Text.
  #texts(): { node: NodeId; piece: number; start: number; text: Text }[] {
    const out: { node: NodeId; piece: number; start: number; text: Text }[] =
      [];
    const walk = (id: NodeId): void => {
      const n = this.nodes.get(id);
      const el = this.byId.get(id);
      if (!n || !el) return;
      n.pieces.forEach((p, i) => {
        if (typeof p !== 'string') {
          walk(p);
          return;
        }
        for (const t of el.textsOf(i))
          out.push({
            node: id,
            piece: i,
            start: startOfText.get(t) ?? 0,
            text: t,
          });
      });
    };
    if (this.root) walk(this.root.nodeId);
    return out;
  }

  // Selection from second end (VM) to cursor: rectangles of lines of text.
  showSelection(s: Selection): void {
    this.#selection = s;
    this.selectionLayer.replaceChildren();
    const c = this.#cursor;
    const cn = c?.node ?? null;
    if (s.node === null || !c || cn === null) return;
    const texts = this.#texts();
    // the segment of the piece with the offset place (the end of the segment is the beginning of the next) and the displacement in it
    const at = (node: NodeId, piece: number, offset: number) => {
      let i = -1;
      for (const [k, t] of texts.entries())
        if (
          t.node === node &&
          t.piece === piece &&
          (i < 0 || t.start <= offset)
        )
          i = k;
      return { i, off: i < 0 ? 0 : offset - (texts[i]?.start ?? 0) };
    };
    let a = at(s.node, s.piece, s.offset);
    let b = at(cn, c.piece, c.offset);
    if (a.i < 0 || b.i < 0) return;
    if (a.i > b.i || (a.i === b.i && a.off > b.off)) [a, b] = [b, a];
    const origin = this.host.getBoundingClientRect();
    for (const [i, { text: t }] of texts.entries()) {
      if (i < a.i || i > b.i) continue;
      const range = document.createRange();
      range.setStart(t, i === a.i ? utf16Offset(t.data, a.off) : 0);
      range.setEnd(t, i === b.i ? utf16Offset(t.data, b.off) : t.length);
      for (const r of range.getClientRects()) {
        const box = document.createElement('div');
        box.style.left = `${String(r.left - origin.left + this.host.scrollLeft)}px`;
        box.style.top = `${String(r.top - origin.top + this.host.scrollTop)}px`;
        box.style.width = `${String(Math.max(r.width, 4))}px`;
        box.style.height = `${String(r.height)}px`;
        this.selectionLayer.append(box);
      }
    }
  }

  // Place VM (node, piece, offset in code points) → rectangle of the symbol in
  // the coordinates of the view window.
  #box(
    node: NodeId | null,
    piece: number,
    offset: number,
  ): { left: number; top: number; height: number } | undefined {
    const place =
      node === null ? undefined : this.byId.get(node)?.textAt(piece, offset);
    if (!place) return undefined;
    const { text, at } = place;
    const range = document.createRange();
    range.setStart(text, at);
    range.setEnd(text, Math.min(at + 1, text.length));
    const box = range.getBoundingClientRect();
    const origin = this.host.getBoundingClientRect();
    return {
      left: box.left - origin.left + this.host.scrollLeft,
      top: box.top - origin.top + this.host.scrollTop,
      height: box.height,
    };
  }

  get listOpen(): boolean {
    return !this.popup.hidden;
  }

  #cursor: Cursor | undefined;
  get cursor(): Cursor | undefined {
    return this.#cursor;
  }
  #completion: Complete | undefined;

  // Cadet VM. The error path is visible in its entirety (step illumination, other muted)
  // when the cursor or pointer on the node path; the step function (entry point) is only
  // visible with its node.
  #pathAt = { cursor: false, pointer: false };
  static onPath(el: Element | undefined): boolean {
    if (el instanceof ASTNode && el.hasAttribute('path')) return true;
    for (let x = el?.parentElement; x instanceof ASTNode; x = x.parentElement)
      if (x.hasAttribute('path') && x.dataset.kind !== 'n_fn') return true;
    return false;
  }
  #showPath(): void {
    this.host.toggleAttribute(
      'path-focus',
      this.#pathAt.cursor || this.#pathAt.pointer,
    );
  }
  pointPath(el: Element | undefined): void {
    this.#pathAt.pointer = ViewElement.onPath(el);
    this.#showPath();
  }

  showCursor(c: Cursor): void {
    this.#cursor = c;
    this.#pathAt.cursor =
      c.node !== null && ViewElement.onPath(this.byId.get(c.node));
    this.#showPath();
    const box = this.#box(c.node, c.piece, c.offset);
    this.caret.hidden = !box;
    if (!box) return;
    this.caret.style.left = `${String(box.left)}px`;
    this.caret.style.top = `${String(box.top)}px`;
    this.caret.style.height = `${String(box.height)}px`;
    this.caret.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    this.#markLine();
    if (this.#selection) this.showSelection(this.#selection);
  }

  // The cursor line (frame "Laptop"): signatures above node - at the instruction under the cursor: the operator of
  // the line and its operand closest to the carriage (on the line without an operator - all the string signatures).
  #markLine(): void {
    const caret = this.caret.getBoundingClientRect();
    const line: ASTNode[] = [];
    for (const el of this.#marked) {
      el.removeAttribute('on-line');
      // key prompt - at the call, in whose text the carriage (frame "Transition")
      if (el.hasAttribute('keyhint')) {
        const r = el.getBoundingClientRect();
        el.toggleAttribute(
          'at-caret',
          !this.caret.hidden &&
            caret.left >= r.left - 1 &&
            caret.left <= r.right + 1 &&
            caret.top < r.bottom &&
            caret.bottom > r.top,
        );
      }
      if (!el.hasAttribute('use') || this.caret.hidden) continue;
      const r = el.getBoundingClientRect();
      const mid = r.top + Math.min(r.height, caret.height) / 2;
      if (mid > caret.top && mid < caret.bottom) line.push(el);
    }
    const ops = line.filter((el) => el.dataset.kind === 'n_op');
    const gap = (el: ASTNode) => {
      const r = el.getBoundingClientRect();
      return caret.left < r.left
        ? r.left - caret.left
        : Math.max(0, caret.left - r.right);
    };
    const op = ops.sort((a, b) => gap(a) - gap(b)).at(0);
    for (const el of line)
      if (!op || op.contains(el)) el.setAttribute('on-line', '');
  }

  // Lines of text for numbers: whether there is code in a line - not spaces and not a comment (Span tags
  // from VM; text sheet - always code).
  lines(): { code: boolean; draft: boolean; path: number; layer: string }[] {
    const comments = new Map<NodeId, [number, number][]>();
    for (const m of this.#decor?.marks ?? [])
      if (m.kind === 'span' && m.text.startsWith('comment')) {
        const list = comments.get(m.node) ?? [];
        list.push([m.from, m.to]);
        comments.set(m.node, list);
      }
    // steps of the error path: node → 1 (step) or 2 (place of error); string - the one where the node begins
    const steps = new Map<NodeId, number>();
    for (const m of this.#decor?.marks ?? [])
      if (m.kind === 'path') {
        const [i = '', n = ''] = m.text.split('/');
        steps.set(m.node, i === n ? 2 : 1);
      }
    const layers = new Map<NodeId, string>();
    for (const m of this.#decor?.marks ?? [])
      if (m.kind === 'common' || m.kind === 'own' || m.kind === 'own-head')
        layers.set(m.node, m.kind === 'common' ? 'common' : 'own');
    const drafts = new Set(
      (this.#decor?.marks ?? [])
        .filter((m) => m.kind === 'draft')
        .map((m) => m.node),
    );
    const out = [{ code: false, draft: false, path: 0, layer: '' }];
    let inDraft = 0;
    const walk = (id: NodeId): void => {
      const n = this.nodes.get(id);
      if (!n) return;
      if (drafts.has(id)) ++inDraft;
      const step = steps.get(id) ?? 0;
      const here = out[out.length - 1];
      if (step > here.path) here.path = step;
      const layer = layers.get(id);
      if (layer && here.layer === '') here.layer = layer;
      const leaf = n.pieces.every((p) => typeof p === 'string');
      const cs = comments.get(id) ?? [];
      let own = 0;
      for (const p of n.pieces) {
        if (typeof p !== 'string') {
          walk(p);
          continue;
        }
        for (const ch of p) {
          const line = out[out.length - 1];
          if (inDraft > 0) line.draft = true;
          if (ch === '\n')
            out.push({ code: false, draft: false, path: 0, layer: '' });
          else if (
            ch.trim() !== '' &&
            (leaf || !cs.some(([a, b]) => own >= a && own < b))
          )
            line.code = true;
          ++own;
        }
      }
      if (drafts.has(id)) --inDraft;
    };
    walk(this.root?.nodeId ?? this.data.root);
    return out;
  }

  #marked = new Set<ASTNode>();
  // Error locations within node (last labels)
  errorRanges: Range[] = [];

  // [from, to] in the code points of node (its chunks-texts) → Range DOM.
  #rangeIn(id: NodeId, from: number, to: number): Range | undefined {
    const el = this.byId.get(id);
    const n = this.nodes.get(id);
    if (!el || !n) return undefined;
    const range = document.createRange();
    let seen = 0;
    let started = false;
    const texts = n.pieces.flatMap((p, i) =>
      typeof p === 'string' ? el.textsOf(i) : [],
    );
    for (const t of texts) {
      const len = codePoints(t.data);
      if (!started && from < seen + len) {
        range.setStart(t, utf16Offset(t.data, from - seen));
        started = true;
      }
      if (started && to <= seen + len) {
        range.setEnd(t, utf16Offset(t.data, to - seen));
        return range;
      }
      seen += len;
    }
    return undefined;
  }

  // Check marks: old ones are removed, sent. Returns the first error for the status
  // string.
  showMarks(d: Decor): string | undefined {
    this.#decor = d;
    for (const el of this.#marked) {
      el.setMarks({});
      el.removeAttribute('error-range');
    }
    this.#marked.clear();
    const by = new Map<NodeId, NodeMarks>();
    let first: string | undefined;
    for (const m of d.marks) {
      const e = by.get(m.node) ?? {};
      if (m.kind === 'type') e.type = m.text;
      if (m.kind === 'value') e.value = m.text;
      if (m.kind === 'dead') e.dead = true;
      if (m.kind === 'current') e.current = true;
      if (m.kind === 'prev' || m.kind === 'next') e.near = m.kind;
      if (m.kind === 'path') e.path = m.text;
      if (m.kind === 'overloads') e.overloads = m.text;
      if (m.kind === 'chosen') e.chosen = m.text;
      if (m.kind === 'common' || m.kind === 'own') e.line = m.kind;
      if (m.kind === 'diverged') e.diverged = m.text;
      if (m.kind === 'unknown') e.unknown = true;
      if (m.kind === 'role') e.role = m.text;
      if (m.kind === 'use') e.use = m.text;
      if (m.kind === 'mismatch') e.mismatch = m.text;
      if (m.kind === 'keyhint') e.keyhint = m.text;
      if (m.kind === 'other') e.other = true;
      if (m.kind === 'own-part') e.ownPart = true;
      if (m.kind === 'span')
        (e.spans ??= []).push({ from: m.from, to: m.to, kind: m.text });
      if (m.kind === 'error') {
        e.error = m.text;
        first ??= m.text;
      }
      by.set(m.node, e);
    }
    for (const [id, e] of by) {
      const el = this.byId.get(id);
      if (!el) continue;
      el.setMarks(e);
      this.#marked.add(el);
    }
    // error place inside node (raw block: atom and displacement in it) - text range;
    // backlight - CSS Custom Highlight, without editing DOM node
    this.errorRanges = d.marks.flatMap((m) =>
      m.kind === 'error' && m.to > m.from
        ? (this.#rangeIn(m.node, m.from, m.to) ?? [])
        : [],
    );
    const registry = (globalThis.CSS as typeof CSS | undefined)?.highlights;
    if (registry) {
      registry.set('vish-error', new Highlight(...this.errorRanges));
      for (const m of d.marks)
        if (m.kind === 'error' && m.to > m.from)
          this.byId.get(m.node)?.setAttribute('error-range', '');
    }
    // icons shift text - cursor and list are put again
    if (this.#cursor) this.showCursor(this.#cursor);
    if (this.#completion) this.showCompletion(this.#completion);
    return first;
  }

  // The list of additions - under the beginning of the typed word; selected - from VM.
  showCompletion(m: Complete): void {
    this.#completion = m;
    const box = this.#box(m.node, m.piece, m.offset);
    this.popup.hidden = m.items.length === 0 || !box;
    if (this.popup.hidden || !box) {
      this.preview.hidden = true;
      return;
    }
    this.paintComplete(this.popup, m);
    this.popup.style.left = `${String(box.left)}px`;
    // The subject can ask for a list above the line (data-place="above", frame "draft"), if there is space there.
    const above = box.top - this.popup.offsetHeight - 6;
    this.popup.style.top = `${String(
      this.popup.dataset.place === 'above' && above >= this.host.scrollTop
        ? above
        : box.top + box.height,
    )}px`;
    this.popup
      .querySelector('[selected]')
      ?.scrollIntoView({ block: 'nearest' });
    this.preview.hidden = m.preview === '';
    if (this.preview.hidden) return;
    const head = document.createElement('div');
    head.className = 'head';
    head.textContent = 'будет создано';
    const code = document.createElement('pre');
    code.textContent = m.preview.replace(/\n$/, '');
    const note = document.createElement('div');
    note.className = 'note';
    note.textContent =
      'Внутренняя: в базе и в исполнении, в модуле не видна. Откроется во второй панели.';
    this.preview.replaceChildren(head, code, note);
    this.preview.style.left = `${String(box.left + this.popup.offsetWidth + 8)}px`;
    this.preview.style.top = `${String(box.top + box.height)}px`;
  }

  // Click point → place in the text node (indicator for VM).
  placeAt(x: number, y: number): (PieceText & { offset: number }) | undefined {
    const shadowRoots = [...this.byId.values()]
      .filter((n) => n instanceof RecordNode && n.composite)
      .flatMap((n) => (n.shadowRoot ? [n.shadowRoot] : []));
    const pos = document.caretPositionFromPoint(x, y, { shadowRoots });
    if (!pos || !(pos.offsetNode instanceof Text)) return undefined;
    const where = pieceOfText.get(pos.offsetNode);
    if (!where) return undefined;
    return {
      ...where,
      offset:
        (startOfText.get(pos.offsetNode) ?? 0) +
        codePoints(pos.offsetNode.data.slice(0, pos.offset)),
    };
  }
}

export { codePoints, utf16Offset } from './nodes/ast_node';

// Lines of hint: view - signature, link - by click (open). Items of the supplement
// list: name, detail and type; selected - by attribute.
export function renderComplete(el: HTMLElement, m: Complete): void {
  el.replaceChildren(
    ...m.items.map((item, i) => {
      const li = document.createElement('li');
      li.toggleAttribute('selected', i === m.selected);
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = item.name;
      const detail = document.createElement('span');
      detail.className = 'detail';
      detail.textContent = `${item.detail} ${item.kind}`.trim();
      li.append(name, detail);
      return li;
    }),
  );
}

export function renderLines(
  box: HTMLElement,
  lines: readonly HintLine[],
  open: (node: NodeId) => void,
): void {
  box.replaceChildren(
    ...lines.map((l) => {
      const row = document.createElement('div');
      row.className = 'line';
      row.dataset.kind = l.kind;
      const kind = document.createElement('span');
      kind.className = 'kind';
      kind.textContent = l.kind;
      const text = document.createElement('span');
      text.className = 'text';
      if (l.to > l.from) {
        // eslint-disable-next-line @typescript-eslint/no-misused-spread
        const cps = [...l.text];
        const mark = document.createElement('b');
        mark.textContent = cps.slice(l.from, l.to).join('');
        text.append(
          cps.slice(0, l.from).join(''),
          mark,
          cps.slice(l.to).join(''),
        );
      } else text.textContent = l.text;
      row.append(kind, text);
      const target = l.target;
      if (target !== null) {
        row.classList.add('link');
        row.addEventListener('mousedown', (e) => {
          e.preventDefault();
          e.stopPropagation();
          open(target);
        });
      }
      return row;
    }),
  );
}
