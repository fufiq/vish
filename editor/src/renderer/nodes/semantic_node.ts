import type { Piece } from '../../protocol/messages';
import { ASTNode } from './ast_node';

// Sheet (name, literal, hole): its text is the content, not the scenery, so it lies in
// the light DOM (smisl: real text - in the light DOM).
export abstract class SemanticNode extends ASTNode {
  // There are no children at the leaf - the function of the child is not necessary
  override show(pieces: readonly Piece[]): void {
    this.replaceChildren(
      ...pieces.map((p, i) =>
        this.pieceText(i, typeof p === 'string' ? p : ''),
      ),
    );
  }
}
