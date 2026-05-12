import { Component } from 'solid-js';
import { sopGraph } from '../../stores/sops';
import { findNodeRecursive, paramChannels } from '../../lib/sop_graph';
import {
  deleteKeyAtPlayhead,
  setKeyAtPlayhead,
  timeline,
} from '../../stores/timeline';
import './KeyframeDot.css';

interface Props {
  nodeUid: number | null | undefined;
  paramName: string;
  component: number;        // 0/1/2 (x/y/z) for vec3, 0 for scalar
  // The current value to write when the user adds a key. ActorProperties
  // passes the live transform component; for material params this would be
  // the slider value.
  value: () => number;
}

// Houdini-style keyframe indicator next to a parameter input.
//   • dim dot      = no animation on this channel
//   • outlined dot = animated, but no key at current playhead
//   • filled dot   = key sits at current frame (within ½ frame tolerance)
//
// Click toggles a keyframe at the playhead. Renders a no-op disabled button
// when the actor has no source SOP node — keeps the inspector layout stable
// across animatable / non-animatable actors.
export const KeyframeDot: Component<Props> = (props) => {
  const node = () => {
    if (props.nodeUid == null) return undefined;
    // Subnet children live in nested subgraphs, so we need a recursive walk
    // — uids are globally unique across nesting.
    return findNodeRecursive(sopGraph(), props.nodeUid) ?? undefined;
  };
  const channel = () => {
    const n = node();
    if (!n) return null;
    const p = n.params[props.paramName];
    if (!p) return null;
    const chs = paramChannels(p);
    return chs?.[props.component] ?? null;
  };

  const animated = () => {
    const ch = channel();
    return !!ch && ch.keys.length > 0;
  };

  const keyHere = () => {
    const ch = channel();
    if (!ch) return false;
    const t = timeline().current_time;
    const eps = 0.5 / Math.max(timeline().fps, 1);
    return ch.keys.some((k) => Math.abs(k.t - t) < eps);
  };

  const onClick = async (e: MouseEvent) => {
    e.preventDefault();
    if (props.nodeUid == null) return;
    try {
      if (keyHere()) {
        await deleteKeyAtPlayhead({
          nodeUid: props.nodeUid,
          paramName: props.paramName,
          component: props.component,
        });
      } else {
        await setKeyAtPlayhead({
          nodeUid: props.nodeUid,
          paramName: props.paramName,
          component: props.component,
          value: props.value(),
        });
      }
    } catch (err) {
      console.warn('keyframe edit failed:', err);
    }
  };

  const title = () => {
    if (props.nodeUid == null) return 'Not animatable (no SOP source)';
    if (keyHere()) return 'Remove keyframe at current frame';
    if (animated()) return 'Set keyframe at current frame';
    return 'Start animating this channel (sets a keyframe here)';
  };

  return (
    <button
      type="button"
      disabled={props.nodeUid == null}
      class={
        'keyframe-dot' +
        (animated() ? ' is-animated' : '') +
        (keyHere() ? ' on-key' : '')
      }
      title={title()}
      onClick={onClick}
    >
      <span class="keyframe-diamond" />
    </button>
  );
};
