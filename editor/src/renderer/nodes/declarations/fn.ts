import { BlockNode } from '../block_node';

// n_fn is a function of `f(a, b) { … }` or `f(a) => e;`.
export class Fn extends BlockNode {}
customElements.define('tz-fn', Fn);
