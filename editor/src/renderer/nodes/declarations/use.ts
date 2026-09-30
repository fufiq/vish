import { BlockNode } from '../block_node';

// n_use - `use geo.vec;` / `use geo.vec.{norm, dot};` - Module as semantic inclusion.
export class Use extends BlockNode {}
customElements.define('tz-use', Use);
