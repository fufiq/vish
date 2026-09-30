import { SemanticNode } from '../semantic_node';

// n_native is the body of a function in C++ `native "cpp" { … }` (§3.3): the text as
// is, is collected in VM (tz native).
export class Native extends SemanticNode {}
customElements.define('tz-native', Native);
