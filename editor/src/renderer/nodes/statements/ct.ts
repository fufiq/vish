import { BlockNode } from '../block_node';

// n_ct - `ct { … }` is a compilation code.
export class Ct extends BlockNode {}
customElements.define('tz-ct', Ct);
