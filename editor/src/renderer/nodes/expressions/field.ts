import { BlockNode } from '../block_node';

// n_field - `v.[f]` is the field of value in compilation.
export class Field extends BlockNode {}
customElements.define('tz-field', Field);
