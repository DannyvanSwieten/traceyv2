import {
  Component,
  For,
  Index,
  Show,
  createEffect,
  createMemo,
  createSignal,
  onCleanup,
  onMount,
} from 'solid-js';
import * as api from '../../lib/api';
import { AnimatedChannel, channelScope, listAnimatedChannels } from '../../lib/animated_channels';
import { autoTangent, channelValueRange, evalChannel, evalSegment } from '../../lib/curve_eval';
import { Extrap, Keyframe } from '../../lib/sop_graph';
import { animPanelMode, setAnimPanelMode } from '../../lib/anim_panel_mode';
import { sopGraph } from '../../stores/sops';
import {
  currentFrame,
  frameForSeconds,
  secondsForFrame,
  seekFrame,
  timeline,
} from '../../stores/timeline';
import './CurveEditor.css';

// F-curve editor for the bottom animation panel (toggled with the dopesheet
// via anim_panel_mode). Time on x (frames, like the dopesheet), value on y
// with pan/zoom. Curves are plotted from a JS mirror of the engine's channel
// evaluation (lib/curve_eval.ts) so what you see is exactly what plays back.
//
// Interaction summary:
//   wheel            zoom value axis around cursor
//   cmd/ctrl+wheel   zoom both axes around cursor
//   drag empty area  pan (middle-drag pans from anywhere)
//   shift+drag empty box-select keys
//   drag key         move in time+value (snaps to frames; cmd/ctrl = free)
//   drag handle      bezier tangent (unified; alt = break in/out)
//   F                fit visible channels
//   Delete           delete selected keys
//   right-click key  interpolation / delete menu
//   right-click curve / channel row   extrapolation + channel menu

const CHANNEL_LIST_WIDTH = 180; // mirrored in CSS
const RULER_H = 22;             // frame ruler band at the top of the SVG
const GUTTER_W = 44;            // value-label gutter on the left of the SVG
const HANDLE_LEN = 40;          // tangent handle screen length, px
const KEY_R = 3.5;

// X/Y/Z colour convention for vec3 components; scalars hash into a palette.
const VEC3_COLORS = ['#e5484d', '#46a758', '#3e63dd'];
const SCALAR_PALETTE = ['#e2a336', '#9a6ee2', '#36c5c5', '#d36ba6', '#98a03c', '#c4724a'];

export function channelId(ch: AnimatedChannel): string {
  return `${ch.nodeUid}:${ch.paramName}:${ch.component}`;
}

function channelColor(ch: AnimatedChannel): string {
  if (ch.slots === 3) return VEC3_COLORS[ch.component] ?? VEC3_COLORS[0];
  let h = 0;
  const id = channelId(ch);
  for (let i = 0; i < id.length; ++i) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return SCALAR_PALETTE[h % SCALAR_PALETTE.length];
}

// 1-2-5×10ⁿ "nice" step ≥ rough. Generic version of the dopesheet's tick
// logic so it works at any zoom (fractional values, thousands of frames).
function niceStep(rough: number): number {
  if (!(rough > 0) || !Number.isFinite(rough)) return 1;
  const mag = Math.pow(10, Math.floor(Math.log10(rough)));
  for (const m of [1, 2, 5, 10]) {
    if (m * mag >= rough) return m * mag;
  }
  return 10 * mag;
}

function ticksInRange(lo: number, hi: number, step: number): number[] {
  const out: number[] = [];
  const start = Math.ceil(lo / step) * step;
  // Guard against float drift producing a tick just below `lo`.
  for (let v = start; v <= hi + step * 1e-6; v += step) out.push(v);
  return out;
}

// Hidden channels + selection survive panel remounts (same pattern as the
// dopesheet's copy/paste clipboard).
const [hiddenIds, setHiddenIds] = createSignal<Set<string>>(new Set());
const [selection, setSelection] = createSignal<Set<string>>(new Set());

const selKeyId = (chId: string, t: number) => `${chId}@${t.toFixed(6)}`;

interface View {
  f0: number;
  f1: number;
  v0: number;
  v1: number;
}

type Drag =
  | { kind: 'pan'; view: View; x: number; y: number }
  | {
      kind: 'key';
      items: { ch: AnimatedChannel; orig: Keyframe; newT: number; newV: number }[];
      x: number;
      y: number;
      moved: boolean;
    }
  | {
      kind: 'tangent';
      ch: AnimatedChannel;
      orig: Keyframe;
      side: 'in' | 'out';
      broken: boolean;
      newIn: number;
      newOut: number;
      moved: boolean;
    }
  | { kind: 'scrub' };

interface KeyMenuState {
  x: number;
  y: number;
  ch: AnimatedChannel;
  key: Keyframe;
}
interface ChannelMenuState {
  x: number;
  y: number;
  ch: AnimatedChannel;
}

export const CurveEditor: Component = () => {
  // Scope to the selected node/actor (see Dopesheet) so a production scene's
  // thousands of channels don't all render at once.
  const channels = createMemo(() => listAnimatedChannels(sopGraph(), channelScope()));
  const visibleChannels = createMemo(() =>
    channels().filter((ch) => !hiddenIds().has(channelId(ch))),
  );

  const [size, setSize] = createSignal({ w: 600, h: 200 });
  const [view, setView] = createSignal<View>({ f0: 1, f1: 100, v0: -1, v1: 1 });

  // Optimistic preview of in-flight edits: rendering reads effectiveKeys so
  // a sop_graph_changed reload mid-drag can't fight the gesture. Cleared by
  // the store-reload effect below once the commit's own broadcast lands.
  const [override, setOverride] = createSignal<Map<string, Keyframe[]> | null>(null);
  const effectiveKeys = (ch: AnimatedChannel): Keyframe[] =>
    override()?.get(channelId(ch)) ?? ch.keys;

  let drag: Drag | null = null;
  createEffect(() => {
    sopGraph(); // track reloads
    if (!drag) setOverride(null);
  });

  // ── view transforms ──────────────────────────────────────────────────
  const plotW = () => Math.max(size().w - GUTTER_W, 1);
  const plotH = () => Math.max(size().h - RULER_H, 1);
  const xForFrame = (f: number) =>
    GUTTER_W + ((f - view().f0) / Math.max(view().f1 - view().f0, 1e-9)) * plotW();
  const frameForX = (x: number) =>
    view().f0 + ((x - GUTTER_W) / plotW()) * (view().f1 - view().f0);
  const yForValue = (v: number) =>
    RULER_H + (1 - (v - view().v0) / Math.max(view().v1 - view().v0, 1e-12)) * plotH();
  const valueForY = (y: number) =>
    view().v0 + (1 - (y - RULER_H) / plotH()) * (view().v1 - view().v0);
  const xForTime = (t: number) => xForFrame(frameForSeconds(t, timeline().fps));
  const timeForX = (x: number) => secondsForFrame(frameForX(x), timeline().fps);

  const fit = () => {
    const t = timeline();
    const ts0 = secondsForFrame(t.frame_start, t.fps);
    const ts1 = secondsForFrame(t.frame_end, t.fps);
    let min = Infinity;
    let max = -Infinity;
    for (const ch of visibleChannels()) {
      const r = channelValueRange(effectiveKeys(ch), ch.pre, ch.post, ts0, ts1);
      min = Math.min(min, ch.paramType === 'bool' ? -0.1 : r.min);
      max = Math.max(max, ch.paramType === 'bool' ? 1.1 : r.max);
    }
    if (!Number.isFinite(min)) {
      min = -1;
      max = 1;
    }
    if (max - min < 1e-9) {
      min -= 1;
      max += 1;
    }
    const pad = (max - min) * 0.1;
    setView({ f0: t.frame_start, f1: t.frame_end, v0: min - pad, v1: max + pad });
  };

  // ── layout / lifecycle ────────────────────────────────────────────────
  let svgWrapEl: HTMLDivElement | undefined;
  let rootEl: HTMLDivElement | undefined;
  let resizeObserver: ResizeObserver | undefined;

  onMount(() => {
    if (svgWrapEl) {
      resizeObserver = new ResizeObserver(() => {
        if (!svgWrapEl) return;
        const r = svgWrapEl.getBoundingClientRect();
        setSize({ w: Math.max(r.width, 1), h: Math.max(r.height, 1) });
      });
      resizeObserver.observe(svgWrapEl);
    }
    fit();
  });
  onCleanup(() => resizeObserver?.disconnect());

  // ── curve path building ───────────────────────────────────────────────
  interface RenderedChannel {
    ch: AnimatedChannel;
    color: string;
    d: string;       // solid path over the keyed range
    dPre: string;    // dashed extrapolation, view-left → first key
    dPost: string;   // dashed extrapolation, last key → view-right
    keys: Keyframe[];
  }

  const sampledPath = (
    keys: Keyframe[],
    pre: Extrap,
    post: Extrap,
    x0: number,
    x1: number,
  ): string => {
    if (x1 - x0 < 1) return '';
    let d = '';
    const STEP_PX = 3;
    for (let x = x0; x <= x1 + STEP_PX * 0.5; x += STEP_PX) {
      const cx = Math.min(x, x1);
      const y = yForValue(evalChannel(keys, pre, post, timeForX(cx)));
      d += (d === '' ? 'M' : 'L') + `${cx.toFixed(1)} ${y.toFixed(1)}`;
    }
    return d;
  };

  const rendered = createMemo<RenderedChannel[]>(() => {
    const s = size();
    void view(); // path geometry depends on the view transform
    const rightX = s.w;
    return visibleChannels().map((ch) => {
      const keys = effectiveKeys(ch);
      const color = channelColor(ch);
      if (keys.length === 0) {
        return { ch, color, d: '', dPre: '', dPost: '', keys };
      }
      if (keys.length === 1) {
        // Single key: flat line across the whole plot.
        const y = yForValue(keys[0].v).toFixed(1);
        return {
          ch,
          color,
          d: `M${GUTTER_W} ${y}L${rightX} ${y}`,
          dPre: '',
          dPost: '',
          keys,
        };
      }
      const firstX = xForTime(keys[0].t);
      const lastX = xForTime(keys[keys.length - 1].t);

      // Solid path over the keyed range, segment-aware.
      let d = '';
      for (let i = 0; i + 1 < keys.length; ++i) {
        const a = keys[i];
        const b = keys[i + 1];
        const xa = xForTime(a.t);
        const xb = xForTime(b.t);
        if (xb < GUTTER_W - 50 || xa > rightX + 50) continue; // off-screen
        const ya = yForValue(a.v);
        const yb = yForValue(b.v);
        if (d === '') d = `M${xa.toFixed(1)} ${ya.toFixed(1)}`;
        if (a.i === 'step') {
          d += `H${xb.toFixed(1)}V${yb.toFixed(1)}`;
        } else if (a.i === 'linear') {
          d += `L${xb.toFixed(1)} ${yb.toFixed(1)}`;
        } else {
          const n = Math.min(Math.max(Math.round((xb - xa) / 4), 4), 64);
          for (let j = 1; j <= n; ++j) {
            const t = a.t + ((b.t - a.t) * j) / n;
            d += `L${xForTime(t).toFixed(1)} ${yForValue(evalSegment(a, b, t)).toFixed(1)}`;
          }
        }
      }

      const dPre =
        firstX > GUTTER_W
          ? sampledPath(keys, ch.pre, ch.post, GUTTER_W, Math.min(firstX, rightX))
          : '';
      const dPost =
        lastX < rightX
          ? sampledPath(keys, ch.pre, ch.post, Math.max(lastX, GUTTER_W), rightX)
          : '';
      return { ch, color, d, dPre, dPost, keys };
    });
  });

  // ── rulers / grid ─────────────────────────────────────────────────────
  const frameTicks = createMemo(() => {
    const v = view();
    const step = niceStep((v.f1 - v.f0) / Math.max(plotW() / 70, 2));
    return ticksInRange(v.f0, v.f1, step);
  });
  const valueTicks = createMemo(() => {
    const v = view();
    const step = niceStep((v.v1 - v.v0) / Math.max(plotH() / 40, 2));
    return ticksInRange(v.v0, v.v1, step);
  });
  const fmtValue = (v: number): string => {
    const span = view().v1 - view().v0;
    const decimals = span > 100 ? 0 : span > 1 ? 1 : span > 0.01 ? 3 : 5;
    return v.toFixed(decimals);
  };

  // ── selection helpers ─────────────────────────────────────────────────
  const isSelected = (ch: AnimatedChannel, k: Keyframe) =>
    selection().has(selKeyId(channelId(ch), k.t));
  const selectOnly = (ch: AnimatedChannel, k: Keyframe) =>
    setSelection(new Set([selKeyId(channelId(ch), k.t)]));
  const toggleSelect = (ch: AnimatedChannel, k: Keyframe) => {
    const next = new Set(selection());
    const id = selKeyId(channelId(ch), k.t);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    setSelection(next);
  };
  const selectedItems = (): { ch: AnimatedChannel; key: Keyframe }[] => {
    const out: { ch: AnimatedChannel; key: Keyframe }[] = [];
    for (const ch of visibleChannels()) {
      const chId = channelId(ch);
      for (const k of effectiveKeys(ch)) {
        if (selection().has(selKeyId(chId, k.t))) out.push({ ch, key: k });
      }
    }
    return out;
  };

  // ── IPC commit helpers ────────────────────────────────────────────────
  const commitKeyEdit = async (
    ch: AnimatedChannel,
    orig: Keyframe,
    next: Keyframe,
  ): Promise<void> => {
    const retimed = Math.abs(next.t - orig.t) > 1e-6;
    const base = {
      nodeUid: ch.nodeUid,
      paramName: ch.paramName,
      component: ch.component,
    };
    if (retimed) {
      await api.paramMoveKeyframe({ ...base, fromTime: orig.t, toTime: next.t });
    }
    if (
      Math.abs(next.v - orig.v) > 1e-9 ||
      Math.abs(next.in - orig.in) > 1e-9 ||
      Math.abs(next.out - orig.out) > 1e-9 ||
      next.i !== orig.i
    ) {
      await api.paramSetKeyframe({
        ...base,
        time: next.t,
        value: next.v,
        interp: next.i,
        inTangent: next.in,
        outTangent: next.out,
      });
    }
  };

  const deleteSelected = async () => {
    const items = selectedItems();
    setSelection(new Set<string>());
    for (const { ch, key } of items) {
      try {
        await api.paramDeleteKeyframe({
          nodeUid: ch.nodeUid,
          paramName: ch.paramName,
          component: ch.component,
          time: key.t,
        });
      } catch (err) {
        console.warn('delete keyframe failed:', err);
      }
    }
  };

  // ── drag machinery ────────────────────────────────────────────────────
  let svgEl: SVGSVGElement | undefined;
  const [boxRect, setBoxRect] = createSignal<{ x0: number; y0: number; x1: number; y1: number } | null>(null);
  let boxAdditive = false;

  const localPos = (e: PointerEvent): { x: number; y: number } => {
    const r = svgEl!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };

  const snapValue = (ch: AnimatedChannel, v: number): number => {
    if (ch.paramType === 'int') return Math.round(v);
    if (ch.paramType === 'bool') return v >= 0.5 ? 1 : 0;
    return v;
  };

  const applyKeyOverride = (
    items: { ch: AnimatedChannel; orig: Keyframe; newT: number; newV: number }[],
  ) => {
    const map = new Map<string, Keyframe[]>();
    // Group edits per channel, then rebuild that channel's key list.
    const byChannel = new Map<string, { ch: AnimatedChannel; edits: typeof items }>();
    for (const it of items) {
      const id = channelId(it.ch);
      if (!byChannel.has(id)) byChannel.set(id, { ch: it.ch, edits: [] });
      byChannel.get(id)!.edits.push(it);
    }
    for (const [id, { ch, edits }] of byChannel) {
      const keys = ch.keys.map((k) => {
        const edit = edits.find((e) => Math.abs(e.orig.t - k.t) < 1e-9);
        return edit ? { ...k, t: edit.newT, v: edit.newV } : { ...k };
      });
      keys.sort((a, b) => a.t - b.t);
      map.set(id, keys);
    }
    setOverride(map);
  };

  const onKeyPointerDown = (e: PointerEvent, ch: AnimatedChannel, k: Keyframe) => {
    if (e.button !== 0) return;
    e.stopPropagation();
    e.preventDefault();
    rootEl?.focus();
    if (e.shiftKey) {
      toggleSelect(ch, k);
      return; // shift-click only adjusts selection; no drag
    }
    if (!isSelected(ch, k)) selectOnly(ch, k);
    const { x, y } = localPos(e);
    drag = {
      kind: 'key',
      items: selectedItems().map(({ ch: c, key }) => ({
        ch: c,
        orig: { ...key },
        newT: key.t,
        newV: key.v,
      })),
      x,
      y,
      moved: false,
    };
    svgEl!.setPointerCapture(e.pointerId);
  };

  const onTangentPointerDown = (
    e: PointerEvent,
    ch: AnimatedChannel,
    k: Keyframe,
    side: 'in' | 'out',
  ) => {
    if (e.button !== 0) return;
    e.stopPropagation();
    e.preventDefault();
    rootEl?.focus();
    drag = {
      kind: 'tangent',
      ch,
      orig: { ...k },
      side,
      broken: e.altKey,
      newIn: k.in,
      newOut: k.out,
      moved: false,
    };
    svgEl!.setPointerCapture(e.pointerId);
  };

  const onSvgPointerDown = (e: PointerEvent) => {
    rootEl?.focus();
    const { x, y } = localPos(e);
    if (e.button === 0 && y < RULER_H) {
      drag = { kind: 'scrub' };
      svgEl!.setPointerCapture(e.pointerId);
      seekFrame(Math.round(frameForX(x))).catch(() => {});
      return;
    }
    if (e.button === 0 && e.shiftKey) {
      boxAdditive = true;
      setBoxRect({ x0: x, y0: y, x1: x, y1: y });
      svgEl!.setPointerCapture(e.pointerId);
      return;
    }
    if (e.button === 0 || e.button === 1) {
      e.preventDefault();
      drag = { kind: 'pan', view: view(), x, y };
      svgEl!.setPointerCapture(e.pointerId);
      if (e.button === 0) setSelection(new Set<string>());
    }
  };

  const onSvgPointerMove = (e: PointerEvent) => {
    const box = boxRect();
    if (box) {
      const { x, y } = localPos(e);
      setBoxRect({ ...box, x1: x, y1: y });
      return;
    }
    if (!drag) return;
    const { x, y } = localPos(e);
    if (drag.kind === 'scrub') {
      seekFrame(Math.round(frameForX(x))).catch(() => {});
      return;
    }
    if (drag.kind === 'pan') {
      const dF = ((drag.x - x) / plotW()) * (drag.view.f1 - drag.view.f0);
      const dV = ((y - drag.y) / plotH()) * (drag.view.v1 - drag.view.v0);
      setView({
        f0: drag.view.f0 + dF,
        f1: drag.view.f1 + dF,
        v0: drag.view.v0 + dV,
        v1: drag.view.v1 + dV,
      });
      return;
    }
    if (drag.kind === 'key') {
      drag.moved = true;
      const fps = timeline().fps;
      const dFrame = ((x - drag.x) / plotW()) * (view().f1 - view().f0);
      const dV = -((y - drag.y) / plotH()) * (view().v1 - view().v0);
      const free = e.metaKey || e.ctrlKey;
      for (const it of drag.items) {
        let f = frameForSeconds(it.orig.t, fps) + dFrame;
        if (!free) f = Math.round(f);
        it.newT = secondsForFrame(f, fps);
        it.newV = snapValue(it.ch, it.orig.v + dV);
      }
      applyKeyOverride(drag.items);
      return;
    }
    if (drag.kind === 'tangent') {
      drag.moved = true;
      const k = drag.orig;
      const kx = xForTime(k.t);
      const ky = yForValue(k.v);
      const sx = (plotW() / (view().f1 - view().f0)) * timeline().fps; // px per second
      const sy = plotH() / (view().v1 - view().v0);                    // px per value
      let hx = x - kx;
      // Clamp to the grabbed side so the handle can't flip across vertical.
      hx = drag.side === 'out' ? Math.max(hx, 4) : Math.min(hx, -4);
      const hy = y - ky;
      const m = (-hy / sy) / (hx / sx);
      drag.broken = drag.broken || e.altKey;
      if (drag.broken) {
        if (drag.side === 'in') drag.newIn = m;
        else drag.newOut = m;
      } else {
        drag.newIn = m;
        drag.newOut = m;
      }
      // Preview via override: same channel, same key, new tangents.
      const newIn = drag.newIn;
      const newOut = drag.newOut;
      const keys = drag.ch.keys.map((kk) =>
        Math.abs(kk.t - k.t) < 1e-9 ? { ...kk, in: newIn, out: newOut } : { ...kk },
      );
      setOverride(new Map([[channelId(drag.ch), keys]]));
      return;
    }
  };

  const onSvgPointerUp = async (e: PointerEvent) => {
    if (svgEl?.hasPointerCapture(e.pointerId)) svgEl.releasePointerCapture(e.pointerId);
    const box = boxRect();
    if (box) {
      setBoxRect(null);
      const xMin = Math.min(box.x0, box.x1);
      const xMax = Math.max(box.x0, box.x1);
      const yMin = Math.min(box.y0, box.y1);
      const yMax = Math.max(box.y0, box.y1);
      const next = boxAdditive && e.shiftKey ? new Set(selection()) : new Set<string>();
      for (const ch of visibleChannels()) {
        const chId = channelId(ch);
        for (const k of effectiveKeys(ch)) {
          const kx = xForTime(k.t);
          const ky = yForValue(k.v);
          if (kx >= xMin && kx <= xMax && ky >= yMin && ky <= yMax) {
            next.add(selKeyId(chId, k.t));
          }
        }
      }
      setSelection(next);
      return;
    }
    const d = drag;
    drag = null;
    if (!d) return;
    if (d.kind === 'pan' || d.kind === 'scrub') return;

    if (d.kind === 'key') {
      if (!d.moved) {
        setOverride(null);
        return;
      }
      // Commit ordered so a moved key can't land on a not-yet-moved selected
      // key: moving right → right-to-left, moving left → left-to-right.
      const movingRight = d.items.some((it) => it.newT > it.orig.t);
      const items = d.items
        .slice()
        .sort((a, b) => (movingRight ? b.orig.t - a.orig.t : a.orig.t - b.orig.t));
      const newSel = new Set<string>();
      try {
        for (const it of items) {
          await commitKeyEdit(it.ch, it.orig, { ...it.orig, t: it.newT, v: it.newV });
          newSel.add(selKeyId(channelId(it.ch), it.newT));
        }
        setSelection(newSel);
      } catch (err) {
        console.warn('key edit commit failed:', err);
        setOverride(null);
      }
      return;
    }

    if (d.kind === 'tangent') {
      if (!d.moved) {
        setOverride(null);
        return;
      }
      try {
        await commitKeyEdit(d.ch, d.orig, { ...d.orig, in: d.newIn, out: d.newOut });
      } catch (err) {
        console.warn('tangent commit failed:', err);
        setOverride(null);
      }
      return;
    }
  };

  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const r = svgEl!.getBoundingClientRect();
    const x = e.clientX - r.left;
    const y = e.clientY - r.top;
    const k = Math.exp(e.deltaY * 0.002);
    const v = view();
    const cv = valueForY(y);
    let { v0, v1 } = v;
    // Value-axis zoom around the cursor; clamp the span.
    const span = (v1 - v0) * k;
    if (span > 1e-6 && span < 1e9) {
      v0 = cv + (v0 - cv) * k;
      v1 = cv + (v1 - cv) * k;
    }
    let { f0, f1 } = v;
    if (e.metaKey || e.ctrlKey) {
      const cf = frameForX(x);
      const fspan = (f1 - f0) * k;
      if (fspan > 0.25 && fspan < 1e7) {
        f0 = cf + (f0 - cf) * k;
        f1 = cf + (f1 - cf) * k;
      }
    }
    setView({ f0, f1, v0, v1 });
  };

  const onRootKeyDown = (e: KeyboardEvent) => {
    const target = e.target as HTMLElement | null;
    if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA')) return;
    if (e.key === 'f' || e.key === 'F') {
      e.preventDefault();
      fit();
    } else if (e.key === 'Delete' || e.key === 'Backspace') {
      e.preventDefault();
      void deleteSelected();
    }
  };

  // ── context menus ─────────────────────────────────────────────────────
  const [keyMenu, setKeyMenu] = createSignal<KeyMenuState | null>(null);
  const [channelMenu, setChannelMenu] = createSignal<ChannelMenuState | null>(null);
  let menuEl: HTMLDivElement | undefined;
  createEffect(() => {
    const m = keyMenu() ?? channelMenu();
    if (!m || !menuEl) return;
    menuEl.style.setProperty('--menu-x', `${m.x}px`);
    menuEl.style.setProperty('--menu-y', `${m.y}px`);
  });
  const closeMenus = () => {
    setKeyMenu(null);
    setChannelMenu(null);
  };
  createEffect(() => {
    if (!keyMenu() && !channelMenu()) return;
    const onDocPointerDown = (e: PointerEvent) => {
      if (menuEl && menuEl.contains(e.target as Node)) return;
      closeMenus();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') closeMenus();
    };
    window.addEventListener('pointerdown', onDocPointerDown, true);
    window.addEventListener('keydown', onKey);
    onCleanup(() => {
      window.removeEventListener('pointerdown', onDocPointerDown, true);
      window.removeEventListener('keydown', onKey);
    });
  });

  const onKeyContextMenu = (e: MouseEvent, ch: AnimatedChannel, k: Keyframe) => {
    e.preventDefault();
    e.stopPropagation();
    setChannelMenu(null);
    setKeyMenu({ x: e.clientX, y: e.clientY, ch, key: { ...k } });
  };
  const onChannelContextMenu = (e: MouseEvent, ch: AnimatedChannel) => {
    e.preventDefault();
    e.stopPropagation();
    setKeyMenu(null);
    setChannelMenu({ x: e.clientX, y: e.clientY, ch });
  };

  // Targets of a key-menu action: the whole selection when the clicked key
  // is part of it, otherwise just the clicked key.
  const keyMenuTargets = (m: KeyMenuState): { ch: AnimatedChannel; key: Keyframe }[] => {
    if (isSelected(m.ch, m.key)) return selectedItems();
    return [{ ch: m.ch, key: m.key }];
  };

  const setInterpForTargets = async (m: KeyMenuState, interp: api.Interp | 'auto') => {
    closeMenus();
    for (const { ch, key } of keyMenuTargets(m)) {
      const keys = effectiveKeys(ch);
      const idx = keys.findIndex((k) => Math.abs(k.t - key.t) < 1e-9);
      let inT = key.in;
      let outT = key.out;
      let mode: api.Interp = interp === 'auto' ? 'bezier' : interp;
      if (interp === 'auto' && idx >= 0) {
        const m2 = autoTangent(keys, idx);
        inT = m2;
        outT = m2;
      }
      try {
        await api.paramSetKeyframe({
          nodeUid: ch.nodeUid,
          paramName: ch.paramName,
          component: ch.component,
          time: key.t,
          value: key.v,
          interp: mode,
          inTangent: inT,
          outTangent: outT,
        });
      } catch (err) {
        console.warn('set interp failed:', err);
      }
    }
  };

  const deleteFromKeyMenu = async (m: KeyMenuState) => {
    closeMenus();
    for (const { ch, key } of keyMenuTargets(m)) {
      try {
        await api.paramDeleteKeyframe({
          nodeUid: ch.nodeUid,
          paramName: ch.paramName,
          component: ch.component,
          time: key.t,
        });
      } catch (err) {
        console.warn('delete keyframe failed:', err);
      }
    }
    setSelection(new Set<string>());
  };

  const setExtrap = async (m: ChannelMenuState, side: 'pre' | 'post', mode: Extrap) => {
    closeMenus();
    try {
      await api.paramSetChannelExtrap({
        nodeUid: m.ch.nodeUid,
        paramName: m.ch.paramName,
        component: m.ch.component,
        ...(side === 'pre' ? { pre: mode } : { post: mode }),
      });
    } catch (err) {
      console.warn('set extrapolation failed:', err);
    }
  };

  const clearChannel = async (m: ChannelMenuState) => {
    closeMenus();
    try {
      await api.paramClearChannel({
        nodeUid: m.ch.nodeUid,
        paramName: m.ch.paramName,
        component: m.ch.component,
      });
    } catch (err) {
      console.warn('clear channel failed:', err);
    }
  };

  const toggleHidden = (ch: AnimatedChannel) => {
    const next = new Set(hiddenIds());
    const id = channelId(ch);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    setHiddenIds(next);
  };

  // ── tangent handle geometry ───────────────────────────────────────────
  interface HandleGeom {
    ch: AnimatedChannel;
    key: Keyframe;
    kx: number;
    ky: number;
    inX: number;
    inY: number;
    outX: number;
    outY: number;
  }
  const handles = createMemo<HandleGeom[]>(() => {
    void view();
    void size();
    const out: HandleGeom[] = [];
    const sx = (plotW() / Math.max(view().f1 - view().f0, 1e-9)) * timeline().fps;
    const sy = plotH() / Math.max(view().v1 - view().v0, 1e-12);
    for (const { ch, key } of selectedItems()) {
      if (key.i !== 'bezier') continue;
      const kx = xForTime(key.t);
      const ky = yForValue(key.v);
      const dir = (m: number): { dx: number; dy: number } => {
        const vx = sx;
        const vy = -m * sy;
        const len = Math.hypot(vx, vy) || 1;
        return { dx: (vx / len) * HANDLE_LEN, dy: (vy / len) * HANDLE_LEN };
      };
      const o = dir(key.out);
      const i = dir(key.in);
      out.push({
        ch,
        key,
        kx,
        ky,
        inX: kx - i.dx,
        inY: ky - i.dy,
        outX: kx + o.dx,
        outY: ky + o.dy,
      });
    }
    return out;
  });

  const fps = () => timeline().fps;

  return (
    <div
      ref={rootEl}
      class="curve-editor"
      tabIndex={0}
      onKeyDown={onRootKeyDown}
    >
      <div class="curve-editor-header">
        {/* Toggle sits in the fixed-width left column so it stays in the
            same screen position as the dopesheet's, making the mode switch
            feel like one control. */}
        <div class="curve-editor-header-left">
          <div class="anim-mode-toggle">
            <button
              type="button"
              classList={{ 'is-active': animPanelMode() === 'dopesheet' }}
              onClick={() => setAnimPanelMode('dopesheet')}
            >
              Dopesheet
            </button>
            <button
              type="button"
              classList={{ 'is-active': animPanelMode() === 'curves' }}
              onClick={() => setAnimPanelMode('curves')}
            >
              Curves
            </button>
          </div>
        </div>
        <span class="curve-editor-header-count">
          {channels().length} channel{channels().length === 1 ? '' : 's'}
        </span>
        <button
          type="button"
          class="curve-editor-fit-btn"
          title="Fit visible channels in view (F)"
          onClick={fit}
        >
          Fit
        </button>
      </div>

      <div class="curve-editor-body">
        <div class="curve-channel-list">
          <For each={channels()}>
            {(ch) => (
              <div
                class="curve-channel-row"
                classList={{ 'is-hidden': hiddenIds().has(channelId(ch)) }}
                title={ch.label}
                onContextMenu={(e) => onChannelContextMenu(e, ch)}
              >
                <button
                  type="button"
                  class="curve-channel-eye"
                  title={hiddenIds().has(channelId(ch)) ? 'Show channel' : 'Hide channel'}
                  onClick={() => toggleHidden(ch)}
                >
                  {hiddenIds().has(channelId(ch)) ? '⊘' : '👁'}
                </button>
                <span
                  class="curve-channel-swatch"
                  ref={(el) => el.style.setProperty('--ch-color', channelColor(ch))}
                />
                <span class="curve-channel-label">{ch.label}</span>
              </div>
            )}
          </For>
          <Show when={channels().length === 0}>
            <div class="curve-editor-empty">
              No animated channels.
              <br />
              Click a keyframe diamond next to a parameter to start animating.
            </div>
          </Show>
        </div>

        <div class="curve-canvas-wrap" ref={svgWrapEl}>
          <svg
            ref={svgEl}
            class="curve-canvas"
            width={size().w}
            height={size().h}
            onPointerDown={onSvgPointerDown}
            onPointerMove={onSvgPointerMove}
            onPointerUp={onSvgPointerUp}
            onWheel={onWheel}
            onContextMenu={(e) => e.preventDefault()}
          >
            <defs>
              <clipPath id="curve-plot-clip">
                <rect
                  x={GUTTER_W}
                  y={RULER_H}
                  width={Math.max(size().w - GUTTER_W, 0)}
                  height={Math.max(size().h - RULER_H, 0)}
                />
              </clipPath>
            </defs>

            {/* value gridlines + labels */}
            <g class="curve-grid">
              <Index each={valueTicks()}>
                {(v) => (
                  <>
                    <line
                      class={Math.abs(v()) < 1e-12 ? 'curve-gridline curve-gridline-zero' : 'curve-gridline'}
                      x1={GUTTER_W}
                      x2={size().w}
                      y1={yForValue(v())}
                      y2={yForValue(v())}
                    />
                    <text class="curve-grid-label" x={GUTTER_W - 5} y={yForValue(v()) + 3}>
                      {fmtValue(v())}
                    </text>
                  </>
                )}
              </Index>
              <Index each={frameTicks()}>
                {(f) => (
                  <line
                    class="curve-gridline curve-gridline-v"
                    x1={xForFrame(f())}
                    x2={xForFrame(f())}
                    y1={RULER_H}
                    y2={size().h}
                  />
                )}
              </Index>
            </g>

            {/* curves */}
            <g clip-path="url(#curve-plot-clip)">
              <For each={rendered()}>
                {(rc) => (
                  <g>
                    <Show when={rc.dPre !== ''}>
                      <path class="curve-path curve-path-extrap" stroke={rc.color} d={rc.dPre} />
                    </Show>
                    <Show when={rc.dPost !== ''}>
                      <path class="curve-path curve-path-extrap" stroke={rc.color} d={rc.dPost} />
                    </Show>
                    <Show when={rc.d !== ''}>
                      <path class="curve-path" stroke={rc.color} d={rc.d} />
                      {/* fat invisible twin for right-click hit area */}
                      <path
                        class="curve-path-hit"
                        d={rc.d}
                        onContextMenu={(e) => onChannelContextMenu(e, rc.ch)}
                      />
                    </Show>
                  </g>
                )}
              </For>

              {/* tangent handles (selected bezier keys) */}
              <g class="curve-handles">
                <For each={handles()}>
                  {(h) => (
                    <>
                      <line class="curve-handle-line" x1={h.kx} y1={h.ky} x2={h.inX} y2={h.inY} />
                      <line class="curve-handle-line" x1={h.kx} y1={h.ky} x2={h.outX} y2={h.outY} />
                      <circle
                        class="curve-handle-dot"
                        cx={h.inX}
                        cy={h.inY}
                        r={3.5}
                        onPointerDown={(e) => onTangentPointerDown(e, h.ch, h.key, 'in')}
                      />
                      <circle
                        class="curve-handle-dot"
                        cx={h.outX}
                        cy={h.outY}
                        r={3.5}
                        onPointerDown={(e) => onTangentPointerDown(e, h.ch, h.key, 'out')}
                      />
                    </>
                  )}
                </For>
              </g>

              {/* key dots */}
              <For each={rendered()}>
                {(rc) => (
                  <For each={rc.keys}>
                    {(k) => (
                      <circle
                        class="curve-key"
                        classList={{ 'is-selected': isSelected(rc.ch, k) }}
                        fill={rc.color}
                        cx={xForTime(k.t)}
                        cy={yForValue(k.v)}
                        r={isSelected(rc.ch, k) ? KEY_R + 1.5 : KEY_R}
                        onPointerDown={(e) => onKeyPointerDown(e, rc.ch, k)}
                        onContextMenu={(e) => onKeyContextMenu(e, rc.ch, k)}
                      >
                        <title>
                          {`f${Math.round(frameForSeconds(k.t, fps()))}: ${k.v.toFixed(3)} (${k.i})`}
                        </title>
                      </circle>
                    )}
                  </For>
                )}
              </For>

              {/* playhead */}
              <line
                class="curve-playhead"
                x1={xForFrame(currentFrame())}
                x2={xForFrame(currentFrame())}
                y1={RULER_H}
                y2={size().h}
              />

              {/* box-select rubber band */}
              <Show when={boxRect()}>
                {(b) => (
                  <rect
                    class="curve-box-select"
                    x={Math.min(b().x0, b().x1)}
                    y={Math.min(b().y0, b().y1)}
                    width={Math.abs(b().x1 - b().x0)}
                    height={Math.abs(b().y1 - b().y0)}
                  />
                )}
              </Show>
            </g>

            {/* frame ruler band (drawn last so it covers curve overdraw) */}
            <g class="curve-ruler">
              <rect class="curve-ruler-bg" x={0} y={0} width={size().w} height={RULER_H} />
              <Index each={frameTicks()}>
                {(f) => (
                  <text class="curve-ruler-label" x={xForFrame(f()) + 3} y={RULER_H - 7}>
                    {Math.round(f())}
                  </text>
                )}
              </Index>
              <line
                class="curve-playhead"
                x1={xForFrame(currentFrame())}
                x2={xForFrame(currentFrame())}
                y1={0}
                y2={RULER_H}
              />
            </g>
          </svg>
        </div>
      </div>

      {/* key context menu */}
      <Show when={keyMenu()}>
        {(m) => (
          <div ref={menuEl} class="curve-ctx-menu">
            <div class="curve-ctx-section-label">Interpolation</div>
            <For
              each={[
                { id: 'step', label: 'Step' },
                { id: 'linear', label: 'Linear' },
                { id: 'bezier', label: 'Bezier' },
                { id: 'auto', label: 'Auto (smooth)' },
              ] as { id: api.Interp | 'auto'; label: string }[]}
            >
              {(opt) => (
                <button
                  type="button"
                  class="curve-ctx-item"
                  classList={{ 'is-current': m().key.i === opt.id }}
                  onClick={() => void setInterpForTargets(m(), opt.id)}
                >
                  <span class="curve-ctx-mark">{m().key.i === opt.id ? '✓' : ''}</span>
                  {opt.label}
                </button>
              )}
            </For>
            <div class="curve-ctx-separator" />
            <button
              type="button"
              class="curve-ctx-item curve-ctx-danger"
              onClick={() => void deleteFromKeyMenu(m())}
            >
              <span class="curve-ctx-mark" />
              Delete
            </button>
          </div>
        )}
      </Show>

      {/* channel context menu */}
      <Show when={channelMenu()}>
        {(m) => (
          <div ref={menuEl} class="curve-ctx-menu">
            <div class="curve-ctx-section-label">{m().ch.label}</div>
            <For each={['pre', 'post'] as const}>
              {(side) => (
                <>
                  <div class="curve-ctx-section-label">
                    {side === 'pre' ? 'Before first key' : 'After last key'}
                  </div>
                  <For each={['hold', 'linear', 'cycle'] as Extrap[]}>
                    {(mode) => (
                      <button
                        type="button"
                        class="curve-ctx-item"
                        classList={{ 'is-current': m().ch[side] === mode }}
                        onClick={() => void setExtrap(m(), side, mode)}
                      >
                        <span class="curve-ctx-mark">{m().ch[side] === mode ? '✓' : ''}</span>
                        {mode[0].toUpperCase() + mode.slice(1)}
                      </button>
                    )}
                  </For>
                </>
              )}
            </For>
            <div class="curve-ctx-separator" />
            <button
              type="button"
              class="curve-ctx-item"
              onClick={() => {
                toggleHidden(m().ch);
                closeMenus();
              }}
            >
              <span class="curve-ctx-mark" />
              {hiddenIds().has(channelId(m().ch)) ? 'Show channel' : 'Hide channel'}
            </button>
            <button
              type="button"
              class="curve-ctx-item curve-ctx-danger"
              onClick={() => void clearChannel(m())}
            >
              <span class="curve-ctx-mark" />
              Clear channel (delete all keys)
            </button>
          </div>
        )}
      </Show>
    </div>
  );
};

export const CURVE_EDITOR_CHANNEL_LIST_WIDTH = CHANNEL_LIST_WIDTH;
