import { BlockNode } from '../block_node';

// n_if — `if(c) a else b`.
export class If extends BlockNode {}
customElements.define('tz-if', If);
