import type { NodeId, Piece } from '../../protocol/messages';
import { ASTNode, badgeSheet, codePoints, type Span } from './ast_node';

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
  // Composite parts-texts: the number of the piece → its span syntax and text
  readonly #syntax = new Map<number, { span: HTMLSpanElement; text: string }>();
  #spans = '';

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
        span.part = 'syntax';
        span.append(this.pieceText(i, p));
        this.#syntax.set(i, { span, text: p });
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

  // Markup from VM: chunks of text are cut into segments; marked - <span part="view">, the rest - in text.
  // Slices are the code points of their chunks-texts in a row.
  protected override paintSpans(spans: readonly Span[]): void {
    super.paintSpans(spans);
    const key = JSON.stringify(spans);
    if (!this.#composite || key === this.#spans) return;
    this.#spans = key;
    const cut = (s: string, a: number, b: number) =>
      // eslint-disable-next-line @typescript-eslint/no-misused-spread
      [...s].slice(a, b).join('');
    let base = 0;
    for (const [i, { span, text }] of [...this.#syntax].sort(
      (a, b) => a[0] - b[0],
    )) {
      const len = codePoints(text);
      const out: Node[] = [];
      let at = 0;
      const plain = (to: number) => {
        if (to > at) out.push(this.pieceText(i, cut(text, at, to), at));
        at = Math.max(at, to);
      };
      for (const s of spans) {
        const from = Math.max(0, s.from - base);
        const to = Math.min(len, s.to - base);
        if (to <= from || from < at) continue;
        plain(from);
        const el = document.createElement('span');
        el.part = s.kind;
        el.className = s.kind;
        el.append(this.pieceText(i, cut(text, from, to), from));
        out.push(el);
        at = to;
      }
      plain(len);
      if (out.length === 0) out.push(this.pieceText(i, '', 0));
      span.replaceChildren(...out);
      base += len;
    }
  }
}
