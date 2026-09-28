import { BlockNode } from '../block_node';

// n_chain is a chain of operations without priorities - just overkill.
export class Chain extends BlockNode {}
customElements.define('tz-chain', Chain);
