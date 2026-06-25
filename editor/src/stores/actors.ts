// Actor list + selection, shared app-wide. The engine owns the truth —
// every SOP cook re-emits actors and broadcasts `scene_changed`; this
// store mirrors it. All refresh paths funnel through refreshActors() so
// the rest of the app never hand-rolls its own getAllActors → setActors
// sequence (the source of the historical drift between callbacks).

import { batch, createSignal } from 'solid-js';
import * as api from '../lib/api';
import type { Actor } from '../components/scene-hierarchy/SceneHierarchy';
import { findNodeRecursive } from '../lib/sop_graph';
import { sopGraph } from './sops';
import { setKeyAtPlayhead } from './timeline';

const [actorsSignal, setActorsSignal] = createSignal<Actor[]>([]);
const [selectedActorIdSignal, setSelectedActorIdSignal] = createSignal<number | null>(null);

export const actors = actorsSignal;
export const setActors = setActorsSignal;
export const selectedActorId = selectedActorIdSignal;
export const setSelectedActorId = setSelectedActorIdSignal;

export function selectedActor(): Actor | null {
  const id = selectedActorIdSignal();
  if (id === null) return null;
  return actorsSignal().find((a) => a.id === id) ?? null;
}

// Pull the current actor list from the engine. `context` labels the
// console warning so a failing refresh says which flow it broke.
export async function refreshActors(context = 'refresh'): Promise<void> {
  try {
    // Capture the selected actor's source SOP node before refreshing — a
    // recook can reassign actor uids (e.g. a material-override edit changes
    // the actor signature → teardown + recreate with a fresh uid). The
    // sop_node_uid IS stable across cooks, so we use it to re-bind the
    // selection and keep the inspector open on the same object.
    const prevId = selectedActorIdSignal();
    const prevSop =
      prevId !== null
        ? actorsSignal().find((a) => a.id === prevId)?.sop_node_uid ?? null
        : null;

    const next = await api.getAllActors();

    batch(() => {
      if (prevId !== null && prevSop != null && !next.some((a) => a.id === prevId)) {
        const rebound = next.find((a) => a.sop_node_uid === prevSop);
        if (rebound) setSelectedActorIdSignal(rebound.id);
      }
      setActorsSignal(next);
    });
  } catch (e) {
    console.warn(`actor ${context} failed:`, e);
  }
}

// Keep the hierarchy live: the server broadcasts `scene_changed` only when
// the actor list/tree actually changes (it gates out per-frame geometry /
// transform recooks). But a SOP that genuinely spawns/kills distinct actors
// every frame (a particle source emitting separate objects) still fires a
// burst. Throttle to a leading-edge tick every FLUSH_MS so the hierarchy
// settles a few times a second instead of 60 — calm, still responsive, and
// eventually consistent (a final change always schedules one more flush).
const SCENE_CHANGED_FLUSH_MS = 100;
export function attachSceneChangedListener(): () => void {
  let pending: ReturnType<typeof setTimeout> | null = null;
  const unlisten = api.listen('scene_changed', () => {
    if (pending !== null) return; // a flush is already scheduled
    pending = setTimeout(() => {
      pending = null;
      void refreshActors('refresh after scene_changed');
    }, SCENE_CHANGED_FLUSH_MS);
  });
  return () => {
    if (pending !== null) clearTimeout(pending);
    unlisten();
  };
}

// Key the selected actor's translate/scale/rotate at the playhead. Used
// by the K hotkey, the WorkspaceBar's "Set Key" button, and the Auto-Key
// toggle after every transform commit.
export function keySelectedActorPose(): void {
  const actor = selectedActor();
  if (!actor || actor.sop_node_uid == null) return;
  const uid = actor.sop_node_uid;
  const tx = actor.transform.position;
  const sc = actor.transform.scale;
  const writes: Promise<void>[] = [
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 0, value: tx.x }),
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 1, value: tx.y }),
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 2, value: tx.z }),
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 0, value: sc.x }),
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 1, value: sc.y }),
    setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 2, value: sc.z }),
  ];
  const node = findNodeRecursive(sopGraph(), uid);
  const rot = node?.params['rotate_euler_deg'];
  if (rot && rot.type === 'vec3') {
    writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 0, value: rot.value[0] }));
    writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 1, value: rot.value[1] }));
    writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 2, value: rot.value[2] }));
  }
  void Promise.allSettled(writes).then((results) => {
    const failed = results.filter((r) => r.status === 'rejected');
    if (failed.length > 0) {
      console.error(`keySelectedActorPose: ${failed.length}/${results.length} keyframe writes failed`,
                    failed.map((f) => (f as PromiseRejectedResult).reason));
    }
  });
}
