import { BlockNode } from '../block_node';

// n_try_compile - SFINAE block: attempt to compile.
export class TryCompile extends BlockNode {}
customElements.define('tz-try-compile', TryCompile);
