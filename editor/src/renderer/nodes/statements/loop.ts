import { BlockNode } from '../block_node';

// n_loop - `loop { … }` - kernel; for/while - prelude.
export class Loop extends BlockNode {}
customElements.define('tz-loop', Loop);
