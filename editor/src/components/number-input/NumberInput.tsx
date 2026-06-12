import { Component, createEffect, createSignal, JSX, on } from 'solid-js';
import './NumberInput.css';

// Reusable numeric input with "type freely" semantics plus the two
// gestures every 3D package trains into your hands:
//
//   • Drag-to-scrub: press on an unfocused field and drag horizontally to
//     scrub the value (step per pixel; Shift = ×0.1 fine, Cmd/Ctrl = ×10
//     coarse). Commits live so the viewport follows the drag. A plain
//     click (no drag) focuses the field for typing, exactly as before.
//   • Arrow keys: ↑/↓ increment by step (same modifiers) and commit
//     immediately — no blur required to see the effect.
//
// The native <input value={...}> binding clobbers the user's in-flight
// text whenever the upstream value re-renders (any time a broadcast
// scene_changed handler refetches actors, or another input commits and
// our parent recomputes). This component avoids that by keeping a local
// signal for the displayed text:
//   • While focused: the input drives `text`. Upstream changes to `value`
//     are ignored — the user owns the field until they leave it.
//   • While blurred: `text` mirrors `value.toFixed(decimals)`, so the
//     field shows the canonical formatted number.
//   • Commit fires on blur AND on Enter, the same gestures the existing
//     transform inputs use.
//
// Numeric formatting is capped at `decimals` digits (default 2) so the
// inspector never shows the 7-digit `toFixed(7)` noise the old transform
// rows displayed.

export interface NumberInputProps {
  value: () => number;
  onCommit: (next: number) => void;
  step?: number;
  min?: number;
  max?: number;
  decimals?: number;
  title?: string;
  id?: string;
  // Solid passes `class` rather than `className` to the JSX host.
  class?: string;
  // Optional callback fired when the user changes focus state. Useful
  // for the keyframe-dot peers that want to highlight while the row is
  // being edited.
  onFocusChange?: (focused: boolean) => void;
}

function format(value: number, decimals: number): string {
  if (!Number.isFinite(value)) return '0';
  return value.toFixed(decimals);
}

// Pixels of horizontal movement before a press becomes a scrub instead of
// a click-to-edit.
const SCRUB_THRESHOLD = 3;

export const NumberInput: Component<NumberInputProps> = (props) => {
  const decimals = () => props.decimals ?? 2;
  const step = () => props.step ?? 0.1;
  const [text, setText] = createSignal(format(props.value(), decimals()));
  const [focused, setFocused] = createSignal(false);
  const [scrubbing, setScrubbing] = createSignal(false);

  let inputRef: HTMLInputElement | undefined;

  // Re-sync the visible text whenever the upstream value changes, but
  // ONLY while the user isn't actively editing the field. Solid's `on`
  // helper makes the dependency explicit so the effect doesn't re-run on
  // every render of the parent.
  createEffect(on(props.value, (v) => {
    if (!focused()) setText(format(v, decimals()));
  }));

  const clamp = (v: number) => {
    if (props.min !== undefined) v = Math.max(props.min, v);
    if (props.max !== undefined) v = Math.min(props.max, v);
    return v;
  };

  const commit = (raw: string) => {
    const trimmed = raw.trim();
    if (trimmed === '' || trimmed === '-' || trimmed === '.') {
      // Reject empty/transient strings — restore the canonical value
      // so the field doesn't sit on an unparseable token.
      setText(format(props.value(), decimals()));
      return;
    }
    const parsed = parseFloat(trimmed);
    if (!Number.isFinite(parsed)) {
      setText(format(props.value(), decimals()));
      return;
    }
    commitNumber(parsed);
  };

  const commitNumber = (v: number) => {
    const next = clamp(v);
    props.onCommit(next);
    // After commit, snap the display to the canonical formatting (the
    // upstream `value` getter will reflect the new number on the next
    // tick; this keeps the field in sync without a flash).
    setText(format(next, decimals()));
  };

  const modScale = (e: { shiftKey: boolean; metaKey: boolean; ctrlKey: boolean }) =>
    e.shiftKey ? 0.1 : e.metaKey || e.ctrlKey ? 10 : 1;

  const onKeyDown: JSX.EventHandler<HTMLInputElement, KeyboardEvent> = (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commit(e.currentTarget.value);
      e.currentTarget.blur();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      setText(format(props.value(), decimals()));
      e.currentTarget.blur();
    } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      // Increment-and-commit so the scene reacts immediately — the native
      // number-input stepping only edits the text, which wouldn't apply
      // until blur. Shift = fine (×0.1), Cmd/Ctrl = coarse (×10).
      e.preventDefault();
      const dir = e.key === 'ArrowUp' ? 1 : -1;
      const current = parseFloat(e.currentTarget.value);
      const base = Number.isFinite(current) ? current : props.value();
      commitNumber(base + dir * step() * modScale(e));
    }
  };

  // ── Drag-to-scrub (value ladder) ───────────────────────────────────
  // Press on an unfocused field: don't focus yet. If the pointer moves
  // past the threshold, scrub; otherwise treat release as click-to-edit.
  const onPointerDown: JSX.EventHandler<HTMLInputElement, PointerEvent> = (e) => {
    if (focused() || e.button !== 0) return;  // editing or non-primary: native behaviour
    e.preventDefault();
    const target = e.currentTarget;
    const pointerId = e.pointerId;
    const startX = e.clientX;
    const startValue = props.value();
    let moved = false;

    const onMove = (mv: PointerEvent) => {
      const dx = mv.clientX - startX;
      if (!moved && Math.abs(dx) < SCRUB_THRESHOLD) return;
      if (!moved) {
        moved = true;
        setScrubbing(true);
        target.setPointerCapture(pointerId);
      }
      commitNumber(startValue + dx * step() * modScale(mv));
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      if (moved) {
        setScrubbing(false);
        if (target.hasPointerCapture(pointerId)) target.releasePointerCapture(pointerId);
      } else {
        // Plain click: enter text-edit mode like before.
        target.focus();
        target.select();
      }
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  };

  return (
    <input
      ref={inputRef}
      type="number"
      id={props.id}
      class={props.class}
      classList={{
        'number-input': true,
        'number-input--scrubbing': scrubbing(),
        'number-input--idle': !focused(),
      }}
      title={props.title}
      step={props.step ?? 0.1}
      min={props.min}
      max={props.max}
      value={text()}
      onInput={(e) => setText(e.currentTarget.value)}
      onPointerDown={onPointerDown}
      // WebKit does NOT cancel the compatibility mousedown when the
      // pointerdown above is preventDefault'ed (unlike Blink/Gecko), so
      // without this the input still focuses and starts a text-selection
      // drag, swallowing the scrub gesture. Suppress it while unfocused —
      // click-to-edit is handled manually in onPointerDown's pointerup.
      onMouseDown={(e) => {
        if (!focused() && e.button === 0) e.preventDefault();
      }}
      onFocus={() => {
        setFocused(true);
        props.onFocusChange?.(true);
      }}
      onBlur={(e) => {
        setFocused(false);
        commit(e.currentTarget.value);
        props.onFocusChange?.(false);
      }}
      onKeyDown={onKeyDown}
    />
  );
};
