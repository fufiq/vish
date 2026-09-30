import { BlockNode } from '../block_node';

// n_pat is a sample of unpacking `array{x, ...xs}`/`point{a, b}` (as a constructor).
export class Pat extends BlockNode {}
customElements.define('tz-pat', Pat);
