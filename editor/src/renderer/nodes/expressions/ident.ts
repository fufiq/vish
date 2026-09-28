import { SemanticNode } from '../semantic_node';

// n_ident is a name.
export class Ident extends SemanticNode {}
customElements.define('tz-ident', Ident);
