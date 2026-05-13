// Renders the hold-Q radial menu as an SVG positioned at the cursor.
// Inert to mouse events — the cursor movement that drives the active-wedge
// state is captured at the App level so the menu doesn't intercept clicks
// on the viewport while open.

import { Component, For, Show } from 'solid-js';
import { pieMenuState } from '../../lib/pie_menu';
import './PieMenu.css';

const OUTER_RADIUS = 90;
const INNER_RADIUS = 30;

export const PieMenu: Component = () => {
  return (
    <Show when={pieMenuState()}>
      {(state) => {
        const s = state();
        const n = s.wedges.length;
        const slice = (Math.PI * 2) / n;
        // Wedges drawn from straight-up (12 o'clock) going clockwise, each
        // centred on its label angle. SVG x grows right, y grows down.
        const wedgePath = (i: number): string => {
          const a0 = i * slice - slice * 0.5 - Math.PI * 0.5;
          const a1 = a0 + slice;
          const x0 = Math.cos(a0) * INNER_RADIUS;
          const y0 = Math.sin(a0) * INNER_RADIUS;
          const x1 = Math.cos(a0) * OUTER_RADIUS;
          const y1 = Math.sin(a0) * OUTER_RADIUS;
          const x2 = Math.cos(a1) * OUTER_RADIUS;
          const y2 = Math.sin(a1) * OUTER_RADIUS;
          const x3 = Math.cos(a1) * INNER_RADIUS;
          const y3 = Math.sin(a1) * INNER_RADIUS;
          return `M ${x0} ${y0} L ${x1} ${y1} A ${OUTER_RADIUS} ${OUTER_RADIUS} 0 0 1 ${x2} ${y2} L ${x3} ${y3} A ${INNER_RADIUS} ${INNER_RADIUS} 0 0 0 ${x0} ${y0} Z`;
        };
        const labelPos = (i: number): [number, number] => {
          const a = i * slice - Math.PI * 0.5;
          const r = (INNER_RADIUS + OUTER_RADIUS) * 0.5;
          return [Math.cos(a) * r, Math.sin(a) * r];
        };
        return (
          <div
            class="pie-menu"
            style={{
              left: `${s.cx - OUTER_RADIUS - 4}px`,
              top: `${s.cy - OUTER_RADIUS - 4}px`,
              width: `${OUTER_RADIUS * 2 + 8}px`,
              height: `${OUTER_RADIUS * 2 + 8}px`,
            }}
          >
            <svg
              width={OUTER_RADIUS * 2 + 8}
              height={OUTER_RADIUS * 2 + 8}
              viewBox={`${-OUTER_RADIUS - 4} ${-OUTER_RADIUS - 4} ${OUTER_RADIUS * 2 + 8} ${OUTER_RADIUS * 2 + 8}`}
            >
              <For each={s.wedges}>
                {(w, i) => (
                  <>
                    <path
                      class="pie-menu-wedge"
                      classList={{ 'pie-menu-wedge--active': s.activeWedge === i() }}
                      d={wedgePath(i())}
                    />
                    <text
                      class="pie-menu-label"
                      classList={{ 'pie-menu-label--active': s.activeWedge === i() }}
                      x={labelPos(i())[0]}
                      y={labelPos(i())[1]}
                      text-anchor="middle"
                      dominant-baseline="central"
                    >
                      {w.label}
                    </text>
                  </>
                )}
              </For>
              {/* Dead-zone disc — visual cue for "release here to cancel". */}
              <circle
                class="pie-menu-deadzone"
                cx={0}
                cy={0}
                r={INNER_RADIUS - 4}
              />
            </svg>
          </div>
        );
      }}
    </Show>
  );
};
