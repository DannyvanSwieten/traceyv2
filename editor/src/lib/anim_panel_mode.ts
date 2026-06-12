// Which editor occupies the bottom animation panel: the dopesheet (key
// timing) or the curve editor (interpolation shaping). Module-level signal
// so the choice survives panel unmount/remount and both panels' header
// toggles share one source of truth.

import { createSignal } from 'solid-js';

export type AnimPanelMode = 'dopesheet' | 'curves';

const [mode, setMode] = createSignal<AnimPanelMode>('dopesheet');

export const animPanelMode = mode;
export const setAnimPanelMode = setMode;
