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
  Complete,
  Inspect,
  TraceStep,
  MergeResult,
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
  key(key: string, mods?: number): void; // key to active presentation (track steps)
  press(key: string, mods?: number): void; // key to the window as from the keyboard (F2 - inspector, Ctrl+P)
  run(): void; // F5 - Launch with arguments and stdin panel
  stop(): void; // Shift+F5
}

// Start state for cap panel and window: idle - not run
export interface RunState {
  state: 'idle' | 'running' | 'done' | 'error' | 'stopped';
  lines: number; // lineage
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
  running: boolean; // is launching (frame “Run”: “ → is coming”)
  conflicts: number; // Unresolved merger conflicts (frame "Merge")
  line: number;
  col: number;
}

// Panel cap: projection ('' - default), whether active, open the same tree nearby.
export interface PaneHead {
  projection: string;
  active: boolean;
  twin: boolean;
}

// The line of the presentation text for numbers: is there a code in it (not spaces and not a comment).
export interface GutterLine {
  code: boolean;
  draft: boolean; // Open draft line (tag draft from VM)
  conflict: boolean; // a line of element with a fusion conflict (tag conflict)
  step: string; // track step on the line: 'current', 'near' or 'neighbor'
  path: number; // Error path step line: 0 - no, 1 - step, 2 - place of error
  layer: string; // Instruction overload: 'common' - as in general logic, 'own' - own, '' - no
  running: boolean; // here is the launch.
}

// What an inspector tree line can do: a rule reference, a node cursor, node backlight in the code.
export interface InspectActions {
  open(node: NodeId): void;
  place(node: NodeId): void;
  mark(node: NodeId | null): void;
  // a tree line - like a node in the code: hovering - card, right click - panel "Edit"
  hover(node: NodeId | null, x: number, y: number): void;
  menu(node: NodeId, x: number, y: number): void;
}

// Panels - draw in their element markup (id from shell) all over again according to the sent.
export interface Panels {
  status: (el: HTMLElement, s: StatusView, host: Host) => void;
  layers: (el: HTMLElement, m: Layers, host: Host) => void;
  clients: (el: HTMLElement, m: Peers, host: Host) => void;
  stops: (el: HTMLElement, m: Stops, host: Host) => void;
  output: (el: HTMLElement, rows: readonly OutRow[], host: Host) => void;
  proposals: (el: HTMLElement, ps: readonly Proposal[], host: Host) => void;
  merge: (el: HTMLElement, m: MergeResult, host: Host) => void;
  // Viewbar cap when the same tree is open in another (frame "projection")
  paneHead: (el: HTMLElement, p: PaneHead) => void;
  // black panel "Edit" (frame "1 · Edit") - by right click: lines "panel" from VM
  cursorBar: (el: HTMLElement, lines: readonly HintLine[], host: Host) => void;
  // taken by merging itself - rows under the module code (frame "Merge")
  mergeNotes: (el: HTMLElement, m: MergeResult) => void;
  header: (el: HTMLElement, h: HeaderView, host: Host) => void;
  gutter: (el: HTMLElement, lines: readonly GutterLine[]) => void;
  // Start panel header: status, buttons (frame "Run")
  runHead: (el: HTMLElement, s: RunState, host: Host) => void;
  // track control lane (frame "Trassa"); null - no track
  traceBar: (el: HTMLElement, t: TraceStep | null, host: Host) => void;
  // guidance and dial, entity card (line "name", "signature", "documentation", "chip ...", "scheme ..." -
  // from VM); open - go to node inspector: tree (focus and path to it are highlighted) and full focus
  // disclosure (in the panel)
  inspect: (
    tree: HTMLElement,
    expand: HTMLElement,
    m: Inspect,
    act: InspectActions,
  ) => void;
  // Disclosure of focus under node in the code (frame "Tree Inspector")
  expansion: (el: HTMLElement, m: Inspect, act: InspectActions) => void;
  // Transition palette (Ctrl+P, F12): list from VM (null - empty) under the input bar, preview "will be created"
  palette: (el: HTMLElement, m: Complete | null, host: Host) => void;
  // Items of the supplement list (place and display - at the window)
  complete: (el: HTMLElement, m: Complete) => void;
  tip: (
    el: HTMLElement,
    lines: readonly HintLine[],
    open: (node: NodeId) => void,
    host: Host,
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
    'mergeNotes',
    'paneHead',
    'cursorBar',
    'header',
    'gutter',
    'tip',
    'complete',
    'palette',
    'inspect',
    'expansion',
    'runHead',
    'traceBar',
  ];
  const missing = need.filter((k) => !(k in r.panels));
  if (r.shell === '' || missing.length > 0)
    throw new Error(
      `тема ${name}: нет ${[r.shell === '' ? 'shell' : '', ...missing].filter(Boolean).join(', ')}`,
    );
  return { ...r, panels: r.panels as Panels };
}
