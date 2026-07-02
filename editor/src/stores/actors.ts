// Actor list + selection, shared app-wide. The engine owns the truth —
// every SOP cook re-emits actors and broadcasts `scene_changed`; this
// store mirrors it. All refresh paths funnel through refreshActors() so
// the rest of the app never hand-rolls its own getAllActors → setActors
// sequence (the source of the historical drift between callbacks).

import { batch, createSignal } from 'solid-js';
import * as api from '../lib/api';
import type { Actor } from '../components/scene-hierarchy/SceneHierarchy';

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

// Key the selected actor's transform at the playhead. Used by the K hotkey, the
// "Set Key" button, and the Auto-Key toggle after every transform commit.
//
// Object/transform animation is a SHOT concern: it authors a USD time sample on the
// placed instance (anim.usd). When no shot is open you're editing an ASSET — a static
// recipe — so transform keying is a deliberate no-op (we no longer write SOP transform
// channels onto the asset graph; procedural param animation is keyed on the param in
// the graph, not on the object's pose).
export function keySelectedActorPose(): void {
  const actor = selectedActor();
  if (!actor) return;
  void api
    .getShotState()
    .then((shot) => {
      // Suspended = editing an asset (the actor is a SOP preview, not a shot prim),
      // so transform keying is paused along with the rest of shot authoring.
      if (shot.open && !shot.suspended)
        void api.shotKeyActor(actor.id).catch((e) => console.error('shotKeyActor failed', e));
      // else: editing an asset → static; no transform keying.
    })
    .catch(() => {});
}
