// All types of node: Imports register elements tz-<view>. List of species -
// node_kinds scheme (from node_kind to include/tz/ast.hpp); index.test.ts checks
// that each species is registered.
import './declarations/module';
import './declarations/fn';
import './declarations/param';
import './declarations/var';
import './declarations/extend';
import './declarations/use';
import './statements/block';
import './statements/if';
import './statements/loop';
import './statements/break';
import './statements/continue';
import './statements/return';
import './statements/throw';
import './statements/ct';
import './statements/unpack';
import './expressions/ident';
import './expressions/int';
import './expressions/float';
import './expressions/str';
import './expressions/call';
import './expressions/op';
import './expressions/chain';
import './expressions/lambda';
import './expressions/construct';
import './expressions/quote';
import './expressions/splice';
import './expressions/hole';
import './expressions/pat';
import './expressions/pat_field';
import './expressions/subject';
import './expressions/spread';
import './expressions/native';
import './expressions/ref';

export function tagOf(kind: string): string {
  return `tz-${kind.replace(/^n_/, '').replaceAll('_', '-')}`;
}
