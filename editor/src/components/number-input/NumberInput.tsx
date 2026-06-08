import { Component, createEffect, createSignal, JSX, on } from 'solid-js';

// Reusable numeric input with "type freely" semantics.
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

export const NumberInput: Component<NumberInputProps> = (props) => {
  const decimals = () => props.decimals ?? 2;
  const [text, setText] = createSignal(format(props.value(), decimals()));
  const [focused, setFocused] = createSignal(false);

  // Re-sync the visible text whenever the upstream value changes, but
  // ONLY while the user isn't actively editing the field. Solid's `on`
  // helper makes the dependency explicit so the effect doesn't re-run on
  // every render of the parent.
  createEffect(on(props.value, (v) => {
    if (!focused()) setText(format(v, decimals()));
  }));

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
    props.onCommit(parsed);
    // After commit, snap the display to the canonical formatting (the
    // upstream `value` getter will reflect the new number on the next
    // tick; this keeps the field in sync without a flash).
    setText(format(parsed, decimals()));
  };

  const onKeyDown: JSX.EventHandler<HTMLInputElement, KeyboardEvent> = (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commit(e.currentTarget.value);
      e.currentTarget.blur();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      setText(format(props.value(), decimals()));
      e.currentTarget.blur();
    }
  };

  return (
    <input
      type="number"
      id={props.id}
      class={props.class}
      title={props.title}
      step={props.step ?? 0.1}
      min={props.min}
      max={props.max}
      value={text()}
      onInput={(e) => setText(e.currentTarget.value)}
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
