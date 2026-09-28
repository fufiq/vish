import type { NodeId, Piece } from '../../protocol/messages';

// Where the text of the piece node lies: on Text-node DOM is (node, piece) - for
// the pointer; on (node, piece) - Text - for the cursor.
export interface PieceText {
  node: NodeId;
  piece: number;
}
export const pieceOfText = new WeakMap<Text, PieceText>();

// An AST node as a DOM element: DOM is a direct projection AST (smisl architecture).
// The syntax of node (its chunks) sends VM - the client has no grammar.
export abstract class ASTNode extends HTMLElement {
  // Normal field: `accessor` (the smisl rule) does not execute Node 24 or Chromium
  // without being downgraded, and nodeId does not override inherited properties
  nodeId: NodeId = -1;
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
