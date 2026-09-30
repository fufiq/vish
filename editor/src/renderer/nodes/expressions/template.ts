import { BlockNode } from '../block_node';

// n_template is a template with the tag `fmt"текст {x} текст"`: in {…}, language expressions.
export class Template extends BlockNode {}
customElements.define('tz-template', Template);
