import { BlockNode } from '../block_node';

// n_quote - `quote { … }` is code as a value.
export class Quote extends BlockNode {}
customElements.define('tz-quote', Quote);
