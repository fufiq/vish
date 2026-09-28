import { SemanticNode } from '../semantic_node';

// n_hole - Hole: Undisassembled fragment (§4.1).
export class Hole extends SemanticNode {}
customElements.define('tz-hole', Hole);
