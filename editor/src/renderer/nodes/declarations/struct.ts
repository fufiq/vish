import { BlockNode } from '../block_node';

// n_struct is an aggregate: `struct имя { поля }`.
export class Struct extends BlockNode {}
customElements.define('tz-struct', Struct);
