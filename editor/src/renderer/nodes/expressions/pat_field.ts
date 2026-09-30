import { BlockNode } from '../block_node';

// n_pat_field is an element of the `.поле: подобразец` sample, referring to the value interface.
export class PatField extends BlockNode {}
customElements.define('tz-pat-field', PatField);
