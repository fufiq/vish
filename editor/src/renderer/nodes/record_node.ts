import type { NodeId, Piece } from '../../protocol/messages';
import { ASTNode, badgeSheet } from './ast_node';

const sheet = new CSSStyleSheet();
sheet.replaceSync(`
  :host { display: inline; white-space: pre; }
  .syntax { color: var(--vish-syntax, inherit); }
`);

// Common element node (§4.5, step 11.4): the view of node to the client is not important - the
// syntax (pieces) sends VM from the record that the node is printed (base language,
// projection, second language). ;
// 
//.
//
// Sheet (no children: name, literature, hole, raw block) - text in light DOM, Shadow DOM - only under
// the icon; composite - syntax decoration in Shadow DOM, children - real elements in light DOM, each
// in its slot (smisl architecture).
export class RecordNode extends ASTNode {
  #composite = false;

  get composite(): boolean {
    return this.#composite;
  }

  protected override badgeRoot(): ShadowRoot {
    if (this.shadowRoot) return this.shadowRoot;
    const root = this.attachShadow({ mode: 'open' });
    root.adoptedStyleSheets = [badgeSheet];
    root.append(document.createElement('slot'));
    return root;
  }

  override show(
    pieces: readonly Piece[],
    child: (id: NodeId) => ASTNode,
  ): void {
    this.#composite = pieces.some((p) => typeof p !== 'string');
    if (!this.#composite) {
      this.replaceChildren(
        ...pieces.map((p, i) =>
          this.pieceText(i, typeof p === 'string' ? p : ''),
        ),
      );
      return;
    }
    const shadow = this.shadowRoot ?? this.attachShadow({ mode: 'open' });
    shadow.adoptedStyleSheets = [sheet, badgeSheet];
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
    shadow.replaceChildren(...deco);
    this.replaceChildren(...kids);
  }
}
