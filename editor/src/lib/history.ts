// Bounded undo/redo stack with label-based coalescing.
//
// Mutators record the *pre-mutation* state with an optional label. When two
// consecutive pushes share the same label within `coalesceWindowMs`, the
// second push is dropped — keeping the original "before" state at the top
// of the stack. That folds successive node-drag ticks (`move:<uid>`) or
// keystroke param edits (`param:<uid>:<name>`) into a single logical undo
// step without requiring explicit transaction begin/end calls from the UI.
//
// `T` is whatever shape the caller wants to snapshot — typically the whole
// graph, or a (graph, navigation-path) tuple for the SOP store. Snapshots
// are taken with structuredClone so the caller can safely mutate after
// pushing; cost is dominated by the JSON-cloneable graph payload (tens of
// μs for typical sub-100-node graphs).

export interface HistoryEntry<T> {
  state: T;
  label?: string;
  t: number;
}

export class History<T> {
  private undoStack: HistoryEntry<T>[] = [];
  private redoStack: HistoryEntry<T>[] = [];

  constructor(
    private readonly capacity = 100,
    private readonly coalesceWindowMs = 500,
  ) {}

  // Push the pre-mutation snapshot. If the previous push is "recent + same
  // label", drop this one (the existing entry already represents the start
  // of this logical edit). Always clears the redo stack — a new branch.
  push(state: T, label?: string): void {
    const now = Date.now();
    if (label && this.undoStack.length > 0) {
      const top = this.undoStack[this.undoStack.length - 1];
      if (top.label === label && now - top.t < this.coalesceWindowMs) {
        top.t = now;
        this.redoStack = [];
        return;
      }
    }
    this.undoStack.push({ state: structuredClone(state), label, t: now });
    if (this.undoStack.length > this.capacity) this.undoStack.shift();
    this.redoStack = [];
  }

  // Pop the last "before" snapshot and return it. Caller passes the
  // current state so it can be saved onto the redo stack. Returns null
  // when there's nothing to undo.
  undo(current: T): T | null {
    if (this.undoStack.length === 0) return null;
    const top = this.undoStack.pop()!;
    this.redoStack.push({ state: structuredClone(current), label: top.label, t: Date.now() });
    return top.state;
  }

  redo(current: T): T | null {
    if (this.redoStack.length === 0) return null;
    const top = this.redoStack.pop()!;
    this.undoStack.push({ state: structuredClone(current), label: top.label, t: Date.now() });
    return top.state;
  }

  canUndo(): boolean { return this.undoStack.length > 0; }
  canRedo(): boolean { return this.redoStack.length > 0; }

  undoDepth(): number { return this.undoStack.length; }
  redoDepth(): number { return this.redoStack.length; }

  clear(): void {
    this.undoStack = [];
    this.redoStack = [];
  }
}
