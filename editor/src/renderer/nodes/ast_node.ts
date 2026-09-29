import type { NodeId, Piece } from '../../protocol/messages';

// Where the text of the piece node lies: on Text-node DOM is (node, piece) - for
// the pointer; on (node, piece) - Text - for the cursor.
export interface PieceText {
  node: NodeId;
  piece: number;
}
export const pieceOfText = new WeakMap<Text, PieceText>();

export const badgeSheet = new CSSStyleSheet();
badgeSheet.replaceSync(`
  .badge { color: var(--tz-type, gray); font-size: 0.85em; margin-left: 1px; }
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
  #error = '';
  get note(): string {
    return this.#note;
  }
  set note(text: string) {
    this.#note = text;
    this.toggleAttribute('noted', text !== '');
    this.title = this.#error || this.#note;
  }

  // Verification Tags from VM (s_decor): the type of announced name is an icon after
  // node in its Shadow DOM (like the type icons in smisl), an error is an emphasis and
  // a hint with a trace. The scenery, not the text.
  protected abstract badgeRoot(): ShadowRoot;

  setMarks(type: string | undefined, error: string | undefined): void {
    this.#badge?.remove();
    this.#badge = undefined;
    if (type !== undefined) {
      this.#badge = document.createElement('span');
      this.#badge.className = 'badge';
      this.#badge.textContent = `: ${type}`;
      this.badgeRoot().append(this.#badge);
    }
    this.#error = error ?? '';
    this.toggleAttribute('error', error !== undefined);
    this.title = this.#error || this.#note;
  }
  readonly #texts = new Map<number, Text>();

  connectedCallback(): void {
    this.classList.add('tz-node');
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
