// The theme is the window visual module: not only colors, but also the markup of the window, panels
// and node elements as a whole: the window does not have its own logic, so you can replace everything
// that draws - one border: the panel receives the VM sent and sends ClientMsg through Host. The logic
// of the editor is extended to the language itself ([[editor]] - overload).
//
//
// Theme - folder themes/<name> /index.ts with export default Theme; picked up by itself
// (import.meta.glob), selected -theme <name>. What the theme did not ask - taken from the basic.
import type {
  ClientMsg,
  Conflict,
  HintLine,
  Layers,
  NodeId,
  Peers,
  Proposal,
  Stops,
} from '../protocol/messages';
import type { ASTNode } from './nodes/ast_node';

// What the panel can do: send VM, open the main view node, reconnect.
export interface Host {
  send(m: ClientMsg): void;
  open(node: NodeId): void;
  reconnect(): void;
  goto(text: string): void;
}

// Brush status: state from VM; action - button (“goto signature”, “reconnect”).
export interface StatusView {
  kind: string;
  text: string;
  hint: string;
  action?: string;
  label?: string;
}

// Launch output line: text and signature of your print (main:12, call node).
export interface OutRow {
  text: string;
  label: string;
  node: NodeId | null;
}

// Window cap: program file, test result, cursor position (line: column).
export interface HeaderView {
  file: string;
  errors: number;
  checked: boolean;
  line: number;
  col: number;
}

// The line of the presentation text for numbers: is there a code in it (not spaces and not a comment).
export interface GutterLine {
  code: boolean;
}

// Panels - draw in their element markup (id from shell) all over again according to the sent.
export interface Panels {
  status: (el: HTMLElement, s: StatusView, host: Host) => void;
  layers: (el: HTMLElement, m: Layers, host: Host) => void;
  clients: (el: HTMLElement, m: Peers, host: Host) => void;
  stops: (el: HTMLElement, m: Stops, host: Host) => void;
  output: (el: HTMLElement, rows: readonly OutRow[], host: Host) => void;
  proposals: (el: HTMLElement, ps: readonly Proposal[], host: Host) => void;
  merge: (el: HTMLElement, cs: readonly Conflict[], host: Host) => void;
  header: (el: HTMLElement, h: HeaderView, host: Host) => void;
  gutter: (el: HTMLElement, lines: readonly GutterLine[]) => void;
  // guidance and dial, entity card (line "name", "signature", "documentation", "chip ...", "schema ..." -
  // from VM); open - go to node
  tip: (
    el: HTMLElement,
    lines: readonly HintLine[],
    open: (node: NodeId) => void,
  ) => void;
}

export interface Theme {
  name: string;
  base?: string;
  // Theme styles - after basic styles
  css?: readonly string[];
  // window markup (body content) - with ids that know the window (#app, #side, #status...)
  shell?: string;
  // node (n_fn...) → element; the rest are general RecordNode
  nodes?: Readonly<Record<string, new () => ASTNode>>;
  panels?: Partial<Panels>;
}

// The basic theme, boiled down to one: everything is mandatory.
export interface Resolved {
  name: string;
  css: string[];
  shell: string;
  nodes: Record<string, new () => ASTNode>;
  panels: Panels;
}

export function resolve(
  themes: ReadonlyMap<string, Theme>,
  name: string,
): Resolved {
  const chain: Theme[] = [];
  for (
    let t = themes.get(name);
    t;
    t = t.base ? themes.get(t.base) : undefined
  ) {
    if (chain.includes(t)) break;
    chain.unshift(t);
  }
  if (chain.length === 0) throw new Error(`нет темы ${name}`);
  const r = {
    name,
    css: [] as string[],
    shell: '',
    nodes: {} as Record<string, new () => ASTNode>,
    panels: {} as Partial<Panels>,
  };
  for (const t of chain) {
    r.css.push(...(t.css ?? []));
    r.shell = t.shell ?? r.shell;
    Object.assign(r.nodes, t.nodes);
    Object.assign(r.panels, t.panels);
  }
  const need: (keyof Panels)[] = [
    'status',
    'layers',
    'clients',
    'stops',
    'output',
    'proposals',
    'merge',
    'header',
    'gutter',
    'tip',
  ];
  const missing = need.filter((k) => !(k in r.panels));
  if (r.shell === '' || missing.length > 0)
    throw new Error(
      `тема ${name}: нет ${[r.shell === '' ? 'shell' : '', ...missing].filter(Boolean).join(', ')}`,
    );
  return { ...r, panels: r.panels as Panels };
}
