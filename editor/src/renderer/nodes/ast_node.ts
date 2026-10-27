import type { NodeId, Piece } from '../../protocol/messages';

// Where the text of the piece node lies: on Text-node DOM is (node, piece) - for
// the pointer; on (node, piece) - Text - for the cursor.
export interface PieceText {
  node: NodeId;
  piece: number;
}
export const pieceOfText = new WeakMap<Text, PieceText>();
// The piece can be divided into several Texts (Span markup from VM): from which code point the piece
// begins this Text.
export const startOfText = new WeakMap<Text, number>();

// The segment of his text node [from, to) in the code points of his chunks-texts in a row: view from VM
// (decl, field, comment, keyword) - the theme paints it through ::part(вид).
export interface Span {
  from: number;
  to: number;
  kind: string;
}

export function codePoints(s: string): number {
  // Protocol offsets - in code points (as in VM), not in
  // eslint-disable-next-line @typescript-eslint/no-misused-spread graphemes
  return [...s].length;
}

export function utf16Offset(s: string, cp: number): number {
  let at = 0;
  let n = 0;
  for (const ch of s) {
    if (n++ === cp) return at;
    at += ch.length;
  }
  return at;
}

export interface NodeMarks {
  type?: string;
  value?: string;
  error?: string;
  dead?: boolean;
  current?: boolean; // runway
  near?: 'prev' | 'next'; // the next step of the track of the same depth (ыF10/F10)
  path?: string; // the step of the "i/all" error path (1 - the entry point, the last - the place of error)
  overloads?: string; // function with overload: "i/n|previous |next signature"
  other?: boolean; // call selects not shown overload - dim
  ownPart?: boolean; // part of overload which differs from the general
  chosen?: string; // the call selects overload shown in the second panel ("i/n")
  line?: 'common' | 'own'; // Instruction overload: as in general logic or its own
  diverged?: string;
  mismatch?: string; // Value-cause of error: its type (frame "Error")
  use?: string; // type of use (or operator) - a signature above node on the cursor line
  role?: string; // VM: fn, prelude, native, param N, var N, global N
  spans?: Span[]; // markup of your text (component node)
  unknown?: boolean; // type not deduced - badge "?" // was general, but in general logic it is no longer (checking the consistency of layers)
}

export const badgeSheet = new CSSStyleSheet();
badgeSheet.replaceSync(`
  .badge { color: var(--vish-type, gray); font-size: 0.65em; position: relative; top: 0.45em; margin-left: 1px; white-space: nowrap; }
  .badge.value { color: var(--vish-value, gray); }
  .badge.unknown { display: inline-block; min-width: 1.2em; top: 0; margin-left: 2px; padding: 0 0.2em; border: 1px dashed #15803d; border-radius: 4px; background: color-mix(in srgb, #15803d 6%, transparent); color: #15803d; text-align: center; font-weight: 600; line-height: 1.3; }
  .above { display: none; }
  .layer { display: inline-block; margin-right: 3px; padding: 0 0.35em; border-radius: 999px; font-size: 0.65em; line-height: 1.5; vertical-align: 0.3em; color: var(--vish-value, gray); border: 1px solid color-mix(in srgb, var(--vish-value, gray) 45%, transparent); cursor: pointer; }
  .layer.chosen { margin: 0 0 0 2px; cursor: default; }
  .switch { display: inline-flex; gap: 3px; align-items: center; margin-right: 4px; font-size: 0.7em; color: var(--vish-value, gray); }
  .switch .arrow { cursor: pointer; padding: 0 2px; }
  .step { display: inline-block; min-width: 1.35em; margin-right: 2px; padding: 0 0.3em; border-radius: 999px; font-size: 0.65em; line-height: 1.5; text-align: center; vertical-align: 0.3em; color: white; background: color-mix(in srgb, var(--vish-error, #c0392b) calc(30% + var(--step, 1) * 70%), var(--vish-bg, white)); }
`);

// The icons and syntax in Shadow DOM node are open to themes such as:part(badge | badge value | step | layer)
// | syntax - the theme changes their appearance without touching the elements.

// An AST node as a DOM element: DOM is a direct projection AST (smisl architecture).
// The syntax of node (its chunks) sends VM - the client has no grammar.
export abstract class ASTNode extends HTMLElement {
  // Normal field: `accessor` (the smisl rule) does not execute Node 24 or Chromium
  // without being downgraded, and nodeId does not override inherited properties
  nodeId: NodeId = -1;
  // note VM (cause of the hole) - hint and emphasis, not text
  #note = '';
  #badge: HTMLSpanElement | undefined;
  #above: HTMLSpanElement | undefined;
  #step: HTMLSpanElement | undefined;
  #layer: HTMLSpanElement | undefined;
  #error = '';
  get note(): string {
    return this.#note;
  }
  set note(text: string) {
    this.#note = text;
    this.toggleAttribute('noted', text !== '');
    this.title = this.#error || this.#note;
  }

  // Verification Tags from VM (s_decor): Name type (ads and uses) - signature after node
  // in its Shadow DOM, shallower and below the line, in the text stream (like type icons
  // in smisl): neighboring signatures do not fit on each other. Error: Emphasizing and
  // hinting with a trice. The scenery, not the text.
  protected abstract badgeRoot(): ShadowRoot;

  // m.value - value in the context of the layer (`= v` icon), m.dead - it does not execute
  // (muffle). m.path is the step number of the error path before node, the tone from pale
  // (entry point) to saturated red (error place).
  setMarks(m: NodeMarks): void {
    // Name role and entity number - attributes: theme color[data-role],[data-entity]
    const [role = '', entity = ''] = (m.role ?? '').split(' ');
    if (role === '') delete this.dataset.role;
    else this.dataset.role = role;
    if (entity === '') delete this.dataset.entity;
    else this.dataset.entity = entity;
    this.paintSpans(m.spans ?? []);
    this.#step?.remove();
    this.#step = undefined;
    const [step = 0, total = 1] = (m.path ?? '').split('/').map(Number);
    if (m.path !== undefined) {
      this.#step = document.createElement('span');
      this.#step.className = 'step';
      this.#step.part = 'step';
      this.#step.textContent = String(step);
      this.badgeRoot().prepend(this.#step);
      this.style.setProperty('--step', String(step / total));
    } else this.style.removeProperty('--step');
    this.toggleAttribute('path', m.path !== undefined);
    this.toggleAttribute('error-place', m.path !== undefined && step === total); // The place of error is the last step
    // layers - overload (frame "Layers"): the function has a switch ‹ i/n › with a signature (arrows - show the
    // neighbor in place: the event vish-layer "fn: node"); the call - the chip of the selected overload (part
    // selected selected selected-i - color by number); call not shown - other, your overload - own-part
    this.#layer?.remove();
    this.#layer = undefined;
    if (m.overloads !== undefined) {
      const [at = '', prev = '', next = '', label = ''] =
        m.overloads.split('|');
      const sw = document.createElement('span');
      sw.className = 'switch';
      sw.part = 'switch';
      const arrow = (text: string, to: string, title: string) => {
        const b = document.createElement('span');
        b.className = 'arrow';
        b.part = 'switch-arrow';
        b.textContent = text;
        b.title = title;
        b.addEventListener('mousedown', (e) => {
          e.preventDefault();
          e.stopPropagation();
          this.dispatchEvent(
            new CustomEvent('vish-layer', {
              detail: `fn:${to}`,
              bubbles: true,
              composed: true,
            }),
          );
        });
        return b;
      };
      const [i = '', n = ''] = at.split('/');
      const count = document.createElement('span');
      count.part = 'switch-count';
      const cur = document.createElement('b');
      cur.textContent = i;
      count.append(cur, `/${n}`);
      const name = document.createElement('span');
      name.part = 'switch-label';
      name.textContent = label;
      sw.append(
        arrow('‹', prev, 'предыдущая перегрузка'),
        count,
        arrow('›', next, 'следующая перегрузка'),
        name,
      );
      this.#layer = sw;
      this.badgeRoot().prepend(sw);
    } else if (m.chosen !== undefined) {
      const [i = '', label = ''] = m.chosen.split('|');
      const chip = document.createElement('span');
      chip.className = 'layer chosen';
      chip.part = `chosen chosen-${i}`;
      chip.textContent = label;
      chip.title = 'выбирает эту перегрузку';
      this.#layer = chip;
      this.badgeRoot().append(chip);
    }
    this.toggleAttribute('overloaded', m.overloads !== undefined);
    this.toggleAttribute('other', m.other === true);
    this.toggleAttribute('own-part', m.ownPart === true);
    if (m.line) this.setAttribute('line', m.line);
    else this.removeAttribute('line');
    this.#badge?.remove();
    this.#badge = undefined;
    this.toggleAttribute('mismatch', m.mismatch !== undefined);
    const text = [
      m.unknown ? '?' : (m.type ?? m.mismatch ?? ''),
      m.value === undefined
        ? ''
        : `${m.type === undefined ? '' : ' '}= ${m.value}`,
    ].join('');
    if (text !== '') {
      this.#badge = document.createElement('span');
      this.#badge.className = m.unknown
        ? 'badge unknown'
        : m.value === undefined
          ? 'badge'
          : 'badge value';
      this.#badge.part = this.#badge.className;
      if (m.unknown) this.#badge.title = 'тип не выведен';
      this.#badge.textContent = text;
      this.badgeRoot().append(this.#badge);
    }
    // caption above node (laptop frame): visible when the window places node on-line (cursor line)
    this.#above?.remove();
    this.#above = undefined;
    if (m.use !== undefined) {
      this.#above = document.createElement('span');
      this.#above.className = 'above';
      this.#above.part = 'above';
      this.#above.textContent = m.use;
      this.badgeRoot().append(this.#above);
    }
    this.toggleAttribute('use', m.use !== undefined);
    this.#error = m.error ?? '';
    this.toggleAttribute('error', m.error !== undefined);
    this.toggleAttribute('dead', m.dead === true);
    this.toggleAttribute('current', m.current === true);
    if (m.near) this.setAttribute('near', m.near);
    else this.removeAttribute('near');
    this.toggleAttribute('diverged', m.diverged !== undefined);
    this.title =
      this.#error ||
      (m.diverged ?? '') ||
      (m.dead ? 'не исполняется в этом контексте' : '') ||
      this.#note;
  }
  readonly #texts = new Map<number, Text[]>();

  connectedCallback(): void {
    this.classList.add('vish-node');
  }

  // Show the pieces; child(id) is a child element.
  abstract show(pieces: readonly Piece[], child: (id: NodeId) => ASTNode): void;

  // Marking up your text (component node) - RecordNode.
  protected paintSpans(spans: readonly Span[]): void {
    this.toggleAttribute('spans', spans.length > 0);
  }

  // Texts of the piece in order (one or more - by markup).
  textsOf(piece: number): readonly Text[] {
    return this.#texts.get(piece) ?? [];
  }

  // Place offset piece (code points) → Text and offset UTF-16 in it; the end of the segment is the
  // beginning of the next.
  textAt(
    piece: number,
    offset: number,
  ): { text: Text; at: number } | undefined {
    const ts = this.textsOf(piece);
    for (const [i, t] of ts.entries()) {
      const start = startOfText.get(t) ?? 0;
      if (offset < start + codePoints(t.data) || i === ts.length - 1)
        return { text: t, at: utf16Offset(t.data, offset - start) };
    }
    return undefined;
  }

  // Text piece, starting with the code point start (0 - the piece again).
  protected pieceText(piece: number, text: string, start = 0): Text {
    const t = document.createTextNode(text);
    const list = start === 0 ? [] : (this.#texts.get(piece) ?? []);
    list.push(t);
    this.#texts.set(piece, list);
    pieceOfText.set(t, { node: this.nodeId, piece });
    startOfText.set(t, start);
    return t;
  }
}
