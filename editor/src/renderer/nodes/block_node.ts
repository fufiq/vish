import type { NodeId, Piece } from '../../protocol/messages';
import { ASTNode, badgeSheet } from './ast_node';

const sheet = new CSSStyleSheet();
sheet.replaceSync(`
  :host { display: inline; white-space: pre; }
  .syntax { color: var(--tz-syntax, inherit); }
`);

// Composite node: Syntax (pieces-texts) - decoration in Shadow DOM, children - real
// elements in the light DOM, each in its named slot (smisl: ).
//
export abstract class BlockNode extends ASTNode {
  readonly #shadow = this.attachShadow({ mode: 'open' });

  get shadow(): ShadowRoot {
    return this.#shadow;
  }

  protected override badgeRoot(): ShadowRoot {
    return this.#shadow;
  }

  override show(
    pieces: readonly Piece[],
    child: (id: NodeId) => ASTNode,
  ): void {
    this.#shadow.adoptedStyleSheets = [sheet, badgeSheet];
    const deco: Node[] = [];
    const kids: ASTNode[] = [];
    pieces.forEach((p, i) => {
      if (typeof p === 'string') {
        const span = document.createElement('span');
        span.className = 'syntax';
        span.append(this.pieceText(i, p));
        deco.push(span);
        return;
      }
      const slot = document.createElement('slot');
      slot.name = `c${String(p)}`;
      deco.push(slot);
      const el = child(p);
      el.slot = slot.name;
      kids.push(el);
    });
    this.#shadow.replaceChildren(...deco);
    this.replaceChildren(...kids);
  }
}
