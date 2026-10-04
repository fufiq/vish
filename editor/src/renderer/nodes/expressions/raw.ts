import { SemanticNode } from '../semantic_node';

// n_raw is a raw block of construction, parsed by code in the language (`syntax {r} => f(r);`,
// §4.4): text as written; node from it makes VM in analysis.
export class Raw extends SemanticNode {}
customElements.define('tz-raw', Raw);
