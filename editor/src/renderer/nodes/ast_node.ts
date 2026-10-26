import type { NodeId, Piece } from '../../protocol/messages';

// Where the text of the piece node lies: on Text-node DOM is (node, piece) - for
// the pointer; on (node, piece) - Text - for the cursor.
export interface PieceText {
  node: NodeId;
  piece: number;
}
export const pieceOfText = new WeakMap<Text, PieceText>();

export interface NodeMarks {
  type?: string;
  value?: string;
  error?: string;
  dead?: boolean;
  current?: boolean; // runway
  near?: 'prev' | 'next'; // the next step of the track of the same depth (ыF10/F10)
  path?: string; // the step of the "i/all" error path (1 - the entry point, the last - the place of error)
}

export const badgeSheet = new CSSStyleSheet();
badgeSheet.replaceSync(`
  .badge { color: var(--vish-type, gray); font-size: 0.65em; position: relative; top: 0.45em; margin-left: 1px; white-space: nowrap; }
  .badge.value { color: var(--vish-value, gray); }
  .step { display: inline-block; min-width: 1.35em; margin-right: 2px; padding: 0 0.3em; border-radius: 999px; font-size: 0.65em; line-height: 1.5; text-align: center; vertical-align: 0.3em; color: white; background: color-mix(in srgb, var(--vish-error, #c0392b) calc(30% + var(--step, 1) * 70%), var(--vish-bg, white)); }
`);

// An AST node as a DOM element: DOM is a direct projection AST (smisl architecture).
// The syntax of node (its chunks) sends VM - the client has no grammar.
export abstract class ASTNode extends HTMLElement {
  // Normal field: `accessor` (the smisl rule) does not execute Node 24 or Chromium
  // without being downgraded, and nodeId does not override inherited properties
  nodeId: NodeId = -1;
  // note VM (cause of the hole) - hint and emphasis, not text
  #note = '';
  #badge: HTMLSpanElement | undefined;
  #step: HTMLSpanElement | undefined;
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
    this.#step?.remove();
    this.#step = undefined;
    const [step = 0, total = 1] = (m.path ?? '').split('/').map(Number);
    if (m.path !== undefined) {
      this.#step = document.createElement('span');
      this.#step.className = 'step';
      this.#step.textContent = String(step);
      this.badgeRoot().prepend(this.#step);
      this.style.setProperty('--step', String(step / total));
    } else this.style.removeProperty('--step');
    this.toggleAttribute('path', m.path !== undefined);
    this.#badge?.remove();
    this.#badge = undefined;
    const text = [
      m.type ?? '',
      m.value === undefined
        ? ''
        : `${m.type === undefined ? '' : ' '}= ${m.value}`,
    ].join('');
    if (text !== '') {
      this.#badge = document.createElement('span');
      this.#badge.className = m.value === undefined ? 'badge' : 'badge value';
      this.#badge.textContent = text;
      this.badgeRoot().append(this.#badge);
    }
    this.#error = m.error ?? '';
    this.toggleAttribute('error', m.error !== undefined);
    this.toggleAttribute('dead', m.dead === true);
    this.toggleAttribute('current', m.current === true);
    if (m.near) this.setAttribute('near', m.near);
    else this.removeAttribute('near');
    this.title =
      this.#error ||
      (m.dead ? 'не исполняется в этом контексте' : '') ||
      this.#note;
  }
  readonly #texts = new Map<number, Text>();

  connectedCallback(): void {
    this.classList.add('vish-node');
  }

  // Show the pieces; child(id) is a child element.
  abstract show(pieces: readonly Piece[], child: (id: NodeId) => ASTNode): void;

  textOf(piece: number): Text | undefined {
    return this.#texts.get(piece);
  }

  protected pieceText(piece: number, text: string): Text {
    const t = document.createTextNode(text);
    this.#texts.set(piece, t);
    pieceOfText.set(t, { node: this.nodeId, piece });
    return t;
  }
}
