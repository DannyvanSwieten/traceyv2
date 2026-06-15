// Auto-key mode — single source of truth lives in stores/timeline.ts.
//
// This module used to own a SECOND, independent auto-key signal, which meant
// the editor had two "auto-key" toggles that didn't agree: the playbar "AK"
// button (timeline.autoKey, gating inspector + actor-property edits) and the
// Animation-workspace "Auto-Key" button (this module, gating only G/R/S
// viewport-transform commits). Whichever one you pressed, half the editing
// surface ignored it. We now re-export the timeline signal under the names
// this module historically exposed, so every toggle and every gate —
// playbar, workspace bar, inspector edits, and the G/R/S commit path —
// shares one boolean.
export {
  autoKey as autoKeyEnabled,
  setAutoKey,
  toggleAutoKey,
} from '../stores/timeline';
