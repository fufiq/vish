import { BlockNode } from '../block_node';

// n_require - `require(c)` is a condition of instantiation.
export class Require extends BlockNode {}
customElements.define('tz-require', Require);
