import { Component, createSignal, onCleanup, onMount } from 'solid-js';
import './ResizeHandle.css';

interface ResizeHandleProps {
  direction: 'horizontal' | 'vertical';
  onResize: (delta: number) => void;
  onResizeEnd?: () => void;
}

export const ResizeHandle: Component<ResizeHandleProps> = (props) => {
  const [isDragging, setIsDragging] = createSignal(false);
  let startPos = 0;

  const handleMouseDown = (e: MouseEvent) => {
    e.preventDefault();
    setIsDragging(true);
    startPos = props.direction === 'horizontal' ? e.clientX : e.clientY;
    document.body.style.cursor = props.direction === 'horizontal' ? 'col-resize' : 'row-resize';
    document.body.style.userSelect = 'none';
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!isDragging()) return;

    const currentPos = props.direction === 'horizontal' ? e.clientX : e.clientY;
    const delta = currentPos - startPos;
    startPos = currentPos;

    props.onResize(delta);
  };

  const handleMouseUp = () => {
    if (isDragging()) {
      setIsDragging(false);
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
      props.onResizeEnd?.();
    }
  };

  onMount(() => {
    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);
  });

  onCleanup(() => {
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  });

  return (
    <div
      class={`resize-handle resize-handle-${props.direction} ${isDragging() ? 'dragging' : ''}`}
      onMouseDown={handleMouseDown}
    />
  );
};
