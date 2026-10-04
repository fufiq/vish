import { SemanticNode } from '../semantic_node';

// n_sym is the operator symbol of the raw block (§4.4): an atom is not printed in projection.
export class Sym extends SemanticNode {}
customElements.define('tz-sym', Sym);
