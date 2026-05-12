import { Component, createSignal, onCleanup } from 'solid-js';
import './Splitter.css';

interface SplitterProps {
  // "vertical" = a vertical bar between left/right panels (drag horizontally).
  // "horizontal" = a horizontal bar between top/bottom panels (drag vertically).
  orientation: 'vertical' | 'horizontal';
  // Called per pointermove with the pixel delta since the last move.
  // Direction matches the splitter axis: positive dx = drag right, positive
  // dy = drag down. Consumer decides whether to add or subtract.
  onDrag: (delta: number) => void;
}

export const Splitter: Component<SplitterProps> = (props) => {
  const [dragging, setDragging] = createSignal(false);

  const onPointerDown = (e: PointerEvent) => {
    e.preventDefault();
    setDragging(true);
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  };

  const onPointerMove = (e: PointerEvent) => {
    if (!dragging()) return;
    const delta = props.orientation === 'vertical' ? e.movementX : e.movementY;
    if (delta !== 0) props.onDrag(delta);
  };

  const onPointerUp = (e: PointerEvent) => {
    if (!dragging()) return;
    setDragging(false);
    (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
  };

  // Safety: if pointercancel fires (e.g. touch loss), drop the drag state.
  const onPointerCancel = () => setDragging(false);

  onCleanup(() => setDragging(false));

  return (
    <div
      class={`splitter splitter-${props.orientation}${dragging() ? ' dragging' : ''}`}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerCancel}
      role="separator"
      aria-orientation={props.orientation}
    />
  );
};
