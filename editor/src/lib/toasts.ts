import { createSignal } from 'solid-js';

// App-wide toast notifications — the replacement for window.alert /
// window.confirm, which block the entire native WebView (including the
// render loop) until dismissed. Module-level signals so any code path
// (stores, IPC handlers, components) can raise a toast without prop
// drilling; <ToastHost/> in App renders the stack.

export type ToastKind = 'info' | 'success' | 'error';

export interface ToastAction {
  label: string;
  run: () => void;
}

export interface ToastItem {
  id: number;
  kind: ToastKind;
  message: string;
  // Optional second line rendered smaller/dimmer — file paths, error
  // details. Keeps the headline scannable.
  detail?: string;
  action?: ToastAction;
}

const [toasts, setToasts] = createSignal<ToastItem[]>([]);
export { toasts };

let nextId = 1;
const timers = new Map<number, ReturnType<typeof setTimeout>>();

export function dismissToast(id: number): void {
  const t = timers.get(id);
  if (t !== undefined) {
    clearTimeout(t);
    timers.delete(id);
  }
  setToasts((prev) => prev.filter((x) => x.id !== id));
}

export interface ToastOptions {
  kind?: ToastKind;
  detail?: string;
  action?: ToastAction;
  // 0 = sticky until manually dismissed.
  durationMs?: number;
}

export function showToast(message: string, opts: ToastOptions = {}): number {
  const id = nextId++;
  const kind = opts.kind ?? 'info';
  const item: ToastItem = {
    id,
    kind,
    message,
    detail: opts.detail,
    action: opts.action,
  };
  setToasts((prev) => [...prev, item]);

  // Errors and actionable toasts linger longer — the user has to read
  // a path or decide on the action, not just glance at a confirmation.
  const duration =
    opts.durationMs ?? (kind === 'error' || opts.action ? 10000 : 4000);
  if (duration > 0) {
    timers.set(
      id,
      setTimeout(() => dismissToast(id), duration),
    );
  }
  return id;
}
