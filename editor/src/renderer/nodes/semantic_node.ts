import type { Piece } from '../../protocol/messages';
import { ASTNode, badgeSheet } from './ast_node';

// Sheet (name, literal, hole): its text is the content, not the scenery, so it lies in
// the light DOM (smisl: real text - in the light DOM).
export abstract class SemanticNode extends ASTNode {
  // Shadow DOM sheet - only under the icon (on the first label): the text remains in
  // the light DOM and is shown through the slot.
  protected override badgeRoot(): ShadowRoot {
    if (this.shadowRoot) return this.shadowRoot;
    const root = this.attachShadow({ mode: 'open' });
    root.adoptedStyleSheets = [badgeSheet];
    root.append(document.createElement('slot'));
    return root;
  }

  // There are no children at the leaf - the function of the child is not necessary
  override show(pieces: readonly Piece[]): void {
    this.replaceChildren(
      ...pieces.map((p, i) =>
        this.pieceText(i, typeof p === 'string' ? p : ''),
      ),
    );
  }
}
