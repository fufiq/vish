import { BlockNode } from '../block_node';

// n_var - Advertisements: `T x = e`, `const x = e`, `&e = v[i]`.
export class Var extends BlockNode {}
customElements.define('tz-var', Var);
