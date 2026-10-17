import { BlockNode } from '../block_node';

// n_node is a node of the type declared by the program (struct ast.twice { code x; }): fields are node.
export class ProgramNode extends BlockNode {}
customElements.define('tz-node', ProgramNode);
