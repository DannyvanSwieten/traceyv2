import { Component, For, Show } from 'solid-js';
import { toasts, dismissToast, ToastItem } from '../../lib/toasts';
import './ToastHost.css';

// Renders the toast stack bottom-right, above everything (including the
// dopesheet/playbar). Mounted once in App; state lives in lib/toasts.ts.

const ToastCard: Component<{ toast: ToastItem }> = (props) => (
  <div
    class={`toast toast--${props.toast.kind}`}
    role={props.toast.kind === 'error' ? 'alert' : 'status'}
  >
    <div class="toast-body">
      <div class="toast-message">{props.toast.message}</div>
      <Show when={props.toast.detail}>
        <div class="toast-detail">{props.toast.detail}</div>
      </Show>
      <Show when={props.toast.action}>
        {(action) => (
          <button
            type="button"
            class="toast-action"
            onClick={() => {
              action().run();
              dismissToast(props.toast.id);
            }}
          >
            {action().label}
          </button>
        )}
      </Show>
    </div>
    <button
      type="button"
      class="toast-close"
      aria-label="Dismiss"
      onClick={() => dismissToast(props.toast.id)}
    >
      ×
    </button>
  </div>
);

export const ToastHost: Component = () => (
  <div class="toast-host" aria-live="polite">
    <For each={toasts()}>{(t) => <ToastCard toast={t} />}</For>
  </div>
);
