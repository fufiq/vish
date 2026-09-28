import { BlockNode } from '../block_node';

// n_return — `return e` / `return(caller) e`.
export class Return extends BlockNode {}
customElements.define('tz-return', Return);
