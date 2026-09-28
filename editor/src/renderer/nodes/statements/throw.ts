import { BlockNode } from '../block_node';

// n_throw - `throw e` is an expression.
export class Throw extends BlockNode {}
customElements.define('tz-throw', Throw);
