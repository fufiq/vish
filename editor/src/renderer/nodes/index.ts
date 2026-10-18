import { schema } from '../../protocol/messages';
import { RecordNode } from './record_node';

export function tagOf(kind: string): string {
  return `tz-${kind.replace(/^n_/, '').replaceAll('_', '-')}`;
}

// The element for each type of node (tz-<view>: DOM - direct projection AST, as in smisl) - one
// common class RecordNode: syntax sends VM, classes by type are not written by hand (§4.5, step
// 11.4). Types - node_kinds circuits (from node_kind to include/tz/ast.hpp).
for (const kind of schema.node_kinds) {
  const tag = tagOf(kind);
  if (!customElements.get(tag)) {
    customElements.define(tag, class extends RecordNode {});
  }
}
