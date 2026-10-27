import { schema } from '../../protocol/messages';
import type { ASTNode } from './ast_node';
import { RecordNode } from './record_node';

export function tagOf(kind: string): string {
  return `vish-${kind.replace(/^n_/, '').replaceAll('_', '-')}`;
}

// Element for each type of node (vish-<view>: DOM - direct projection AST, as in smisl) - by default one
// common class RecordNode: syntax sends VM, classes by type are not written by hand (§4.5, step 11.4).
// Types - node_kinds circuits (from node_kind to include/vish/ast.hpp). A theme can give its element a
// cutom before the first presentation: the elements are recorded once.
export function defineNodes(
  custom: Readonly<Record<string, new () => ASTNode>> = {},
): void {
  for (const kind of schema.node_kinds) {
    const tag = tagOf(kind);
    if (!customElements.get(tag))
      customElements.define(tag, custom[kind] ?? class extends RecordNode {});
  }
}
