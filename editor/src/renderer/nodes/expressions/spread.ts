import { BlockNode } from '../block_node';

// n_spread - `...t` in call arguments: Array/cortege is revealed in arguments.
export class Spread extends BlockNode {}
customElements.define('tz-spread', Spread);
