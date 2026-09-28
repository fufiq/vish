import { SemanticNode } from '../semantic_node';

// n_str is a string literal.
export class Str extends SemanticNode {}
customElements.define('tz-str', Str);
