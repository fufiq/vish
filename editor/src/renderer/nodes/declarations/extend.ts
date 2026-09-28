import { BlockNode } from '../block_node';

// n_extend is an extension of entity: `Имя { … }`, followed by `&f(…)`.
export class Extend extends BlockNode {}
customElements.define('tz-extend', Extend);
