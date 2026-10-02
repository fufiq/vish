import { SemanticNode } from '../semantic_node';

// n_ref is a link to the ad (ref(f), generates the ct code): text is the current name of the target.
export class Ref extends SemanticNode {}
customElements.define('tz-ref', Ref);
