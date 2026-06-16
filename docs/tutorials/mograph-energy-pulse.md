# Tutorial: A Looping Energy Pulse Through a Cube Field

Build a C4D-style MoGraph effect from scratch: a flat grid of cubes that a
glowing "pulse" sweeps across, raising and enlarging the clones it passes
over, then loops forever. We'll plot the animation as a curve, set it to
cycle for a seamless loop, and render it with the path tracer to an MP4.

**You'll touch:** a clone source (Points Grid), a clone shape (Cube), two
effectors (Noise + Plain) with a spherical falloff, the keyframe dots, the
curve editor (with cycle extrapolation), and the Render workspace + Export
Video.

**Time:** ~15 minutes. **End result:** a 5-second seamless loop.

---

## The node graph we're building

```
            Cube ─────────────────────┐ (stamp / input 0)
                                       ▼
Points Grid ─► Noise Effector ─► Plain Effector ─► Copy to Points ─► Object Output
                  (ripple)        (traveling pulse,    (template /
                                   sphere falloff)      input 1)
```

The effectors sit *between* the point source and the cloner: each one reads
the incoming points and modulates position / scale / colour, weighted by its
falloff. The cloner stamps a Cube onto every resulting point.

---

## Part 1 — Build the clone rig

1. Open the editor. You start in the **Modeling** workspace (the workspace
   tabs are along the top — `Modeling · Shading · Simulation · Animation ·
   Render`). The SOP graph editor is docked on the right.

2. In the SOP graph toolbar, click the **Add Node…** dropdown. Nodes are
   grouped by category. Under **Generators**, pick **Cube**.

3. Open **Add Node…** again and under **Cloners** pick **Points Grid**.

4. Open it once more and under **Cloners** pick **Copy to Points**.

5. Wire them up by dragging from a node's output port to the next node's
   input port. Copy to Points has **two** inputs — order matters:
   - Drag **Cube → input 0** (`stamp`, the geometry to clone).
   - Drag **Points Grid → input 1** (`template`, the points to clone onto).

6. Make Copy to Points the rendered output: select it and press **O** (or
   right-click it → **Wire to Object Output**). This wires it to the
   `Object Output` node — the graph's render sink.

7. Dial in the grid. Click **Points Grid** to load it into the inspector
   (top of the SOP dock) and set:

   | Param | Value |
   |-----------|-------|
   | `count_x` | `15` |
   | `count_y` | `1` |
   | `count_z` | `15` |
   | `spacing_x` | `0.6` |
   | `spacing_z` | `0.6` |

   That's a flat 15×15 field (225 points) on the ground plane, centred on
   the origin, spanning roughly ±4.2 units.

8. Shrink the clone so the cubes don't overlap. Select **Cube**, set `size`
   to `0.4`.

You should now see a tidy field of 225 small cubes in the viewport. If it
looks like one big cube, you forgot step 8; if nothing appears, re-check the
port order in step 5 (Cube into input 0, Points Grid into input 1).

---

## Part 2 — Add a ripple (Noise Effector)

The Noise Effector samples a 3-D noise field at each clone's position and
nudges it — perfect for organic, non-uniform motion.

1. **Add Node… → Effectors → Noise Effector**.

2. Re-wire the chain so the effector sits between the grid and the cloner:
   - Drag **Points Grid → Noise Effector** input.
   - Drag **Noise Effector → Copy to Points input 1** (`template`). (Dropping
     a new wire onto an input replaces the old one.)

3. Select **Noise Effector** and set:

   | Param | Value | Why |
   |-----------------------|-----------------|-----|
   | `frequency` | `0.35` | Larger noise features → smoother waves |
   | `position_amount` | `[0, 1.5, 0]` | Push clones up/down in Y |
   | `scale_amount` | `0.4` | Vary clone size with the field |
   | `falloff_shape` | `infinite` | Affect every clone (leave the default) |

   `position_amount` and `scale_amount` are vec3 / float fields in the
   inspector — type the values or drag on them to scrub.

The grid now has a gentle rolling-hills look. We'll make it flow in Part 5.

---

## Part 3 — Add the traveling pulse (Plain Effector + sphere falloff)

The Plain Effector applies one uniform transform to every clone — but gated
by a **falloff**, so only clones inside a region are affected. A *spherical*
falloff gives us a localized bulge we can fly across the grid.

1. **Add Node… → Effectors → Plain Effector**.

2. Insert it after the Noise Effector:
   - Drag **Noise Effector → Plain Effector** input.
   - Drag **Plain Effector → Copy to Points input 1** (`template`).

3. Select **Plain Effector**. First set what the pulse *does* to the clones
   it touches:

   | Param | Value |
   |-----------------|-----------------|
   | `position` | `[0, 2.5, 0]` |
   | `scale` | `2.2` |
   | `use_color` | ✔ (check it) |
   | `color` | `[1.0, 0.45, 0.1]` (hot orange) |

4. Now shape the falloff. Set `falloff_shape` to **`sphere`**. New behaviour:
   `falloff_center` is the sphere's centre, `falloff_size` its radii.

   | Param | Value |
   |-----------------|-----------------|
   | `falloff_shape` | `sphere` |
   | `falloff_size` | `[2, 2, 2]` |
   | `falloff_inner` | `0.2` (soft solid core, smooth edge) |
   | `falloff_center`| `[0, 0, 0]` (for now) |

5. **See the falloff while you place it.** Turn on `weight_to_cd` — this
   writes the falloff weight into the clones' colour, so the affected region
   lights up white. Drag `falloff_center` x back and forth and watch the
   bright bubble slide across the grid. Once you understand the reach, turn
   `weight_to_cd` back **off** so your orange colour returns.

At a centred falloff you now have a cluster of raised, enlarged, orange cubes
in the middle of the field — the pulse, frozen. Time to move it.

---

## Part 4 — Frame the shot

1. Add a light. In the **Scene Hierarchy** (left panel) click
   **+ 💡 Add Light** and choose **Dome** for soft, even environment light
   (or **Sun** for hard directional shadows — try both).

2. Frame the camera: orbit in the viewport until you're looking down at the
   field at a shallow angle (a ¾ aerial view sells the wave best). The
   camera position also shows in the right-panel **Camera** controls if you
   want exact numbers.

---

## Part 5 — Animate it into a seamless loop

We'll sweep the pulse across the grid and back, then use the curve editor's
**cycle** extrapolation so it repeats forever with no pop.

### Set the loop length

1. In the **Playbar** (bottom), set the frame range: **start = `1`**,
   **end = `120`** (5 seconds at the default 24 fps).

### Key the sweep

We animate `falloff_center` x with three keys: left edge → right edge → back
to the left edge. Because the first and last values match, a cycle is seamless.

2. Enable **auto-key**: click the **AK** button in the playbar (it turns red).
   Now any param edit writes a keyframe at the playhead.

3. Go to **frame 1** (click ⏮ or type `1` in the current-frame field).
   Select **Plain Effector**, set `falloff_center` to `[-6, 0, 0]`.

4. Go to **frame 60**. Set `falloff_center` to `[6, 0, 0]`.

5. Go to **frame 120**. Set `falloff_center` back to `[-6, 0, 0]`.

6. Turn **AK** off so you don't accidentally key more edits.

> Each `falloff_center` field has a small **keyframe diamond** beside it.
> It's dim when unanimated, hollow when the channel has keys but none at the
> current frame, and filled when a key sits on the playhead. You can click it
> to toggle a key manually instead of using auto-key.

Press **Space** to play. The pulse sweeps right, then back left over 5
seconds — but it stops at the end. Let's make it loop.

### Shape the motion and make it cycle

7. Switch to the **Animation** workspace (top tab) for a taller editor, then
   in the bottom panel's header click the **Curves** toggle (it sits next to
   **Dopesheet**). You're now in the f-curve editor.

8. In the channel list on the left you'll see the animated channels (the
   `falloff_center` x channel will read something like
   `…plain_effector.falloff_center.x`). Press **F** to fit the curve in view.

9. Right now the sweep is linear (sharp turnaround at the peak). Box-select
   all three keys (shift-drag a rectangle around them), then **right-click a
   key → Auto (smooth)**. The curve becomes a smooth ease in/out — the pulse
   now glides and decelerates at each end instead of snapping.

10. Make it loop: **right-click the curve (or its channel row) → Post →
    Cycle**, and also **Pre → Cycle**. The curve now repeats endlessly in
    both directions (you'll see the dashed extrapolation continue past the
    last key).

11. Set the playbar **loop mode** dropdown (labelled `loop`) to **Loop**, and
    press **Space**. The pulse now sweeps back and forth forever, perfectly
    seamless because frame 1 and frame 120 share the same value.

> **Optional — make the ripple flow too.** Select the Noise Effector and,
> with AK on, key `offset` z to `0` at frame 1 and to `4` at frame 120. Set
> that channel's Post to **Cycle** as well. The noise field now scrolls
> continuously underneath the pulse.

---

## Part 6 — Render with the path tracer

1. Click the **PT Preview** button in the top toolbar. The path-traced image
   appears as an inset (top-right of the viewport) — soft shadows, global
   illumination, the works.

2. Switch to the **Render** workspace (top tab). The path tracer takes over
   the full viewport and the **Render** settings panel opens on the right:

   - **Resolution** — leave on `Viewport`, or pick `1080p (1920×1080)`.
   - **Samples** — quality vs. speed (16–8192). Start at the default and
     raise it for the final.
   - **Bounces** — light bounces (1–16); `4`–`8` is plenty here.
   - **High Quality Preset** — one click sets Samples 4096 / Bounces 8 for a
     clean final look.
   - **Reset Accumulation** — restart sampling after you move the camera or
     tweak a material mid-render.

3. Scrub the playhead and watch the image re-accumulate each frame. When a
   still frame looks clean and the motion reads well, you're ready to export.

---

## Part 7 — Export the loop to video

1. Press **⌘E** (or click **Export Video…** in the top toolbar).

2. In the **Export Video** dialog set:

   | Field | Value |
   |------------------|------------------------------|
   | Output | choose a path, e.g. `pulse.mov` |
   | Frame range | `1` to `120` |
   | FPS | `24` |
   | Resolution | `1920 × 1080` (or click **1080p**) |
   | Samples / frame | `128`–`256` for a clean render |
   | Max bounces | `8` |
   | Codec | `H.264` (or `ProRes 422` for editing) |

   > Width and height must both be **even** numbers for H.264 — the presets
   > already are.

3. Click **Export**. A progress bar shows `frame N / total · ms/frame · ETA`.
   When it finishes you have a 5-second seamless loop of an energy pulse
   rolling through your cube field.

---

## Where to take it next

- **Swap the clone shape.** Replace Cube with **Sphere**, **Torus**, or a
  **glTF Import** of your own model — anything on input 0 of Copy to Points
  gets cloned.
- **Add rotation.** Give the Plain Effector a `rotation_deg` so cubes spin as
  the pulse hits them. (Heads-up: rotation makes Copy to Points fall back to
  CPU baking; for big counts swap the cloner for **Instance**, the GPU
  instancing path — it's terminal and emits clones directly, so you don't
  wire it to Object Output.)
- **Randomize it.** Drop a **Random Effector** into the chain with a small
  `rotation_amount_deg` and `scale_amount` for per-clone variation that
  stays put frame to frame (it's seeded and deterministic).
- **Two pulses.** Duplicate the Plain Effector and animate the second one
  sweeping along Z instead of X — where they cross, the effects stack.
- **Box / linear falloff.** Switch `falloff_shape` to `box` for a sharp-edged
  region, or `linear` (with `falloff_axis`) for a directional wipe.

---

## Quick reference — node parameters used here

**Points Grid** (Cloners): `count_x/y/z`, `spacing_x/y/z`.

**Copy to Points** (Cloners): input 0 = `stamp` (shape), input 1 = `template`
(points). `orient_to_normal` aligns clones to point normals (default on).

**Noise Effector** (Effectors): `frequency`, `offset` (vec3 — the animatable
"flow" knob), `seed`, `position_amount`, `rotation_amount_deg`,
`scale_amount`, `use_color`, `color_amount`.

**Plain Effector** (Effectors): `position`, `rotation_deg`, `scale`,
`use_color`, `color`.

**Shared falloff** (on every effector): `falloff_shape`
(`infinite`/`sphere`/`box`/`linear`), `falloff_center`, `falloff_size`,
`falloff_axis`, `falloff_inner` (0–1 soft core), `falloff_invert`,
`strength` (0–1 overall mix), `weight_to_cd` (debug: paint the weight into
colour).
