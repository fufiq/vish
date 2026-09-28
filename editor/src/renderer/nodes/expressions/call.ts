import { BlockNode } from '../block_node';

// n_call is a call; prelude (`for(x : xs) …`) is also a call.
export class Call extends BlockNode {}
customElements.define('tz-call', Call);
