import { BlockNode } from '../block_node';

// n_unpack - `образец = значение` - unpacking (instruction or condition if).
export class Unpack extends BlockNode {}
customElements.define('tz-unpack', Unpack);
