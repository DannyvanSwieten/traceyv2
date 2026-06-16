# Tutorial: A Procedural Title Card — Curves, Sweep & 3D Text

Build a complete motion-graphics shot from the MoGraph Phase 2 toolkit: a ring
of cubes orbiting a glowing swept-tube coil, with an extruded 3D text title in
the center, all spinning in a seamless loop and rendered with the path tracer.

**You'll touch:** the curve generators (Line / Circle / Spiral), **clone-along-
spline** (a curve feeding a cloner directly), the **Resample** modifier, the
**Sweep** node (profile-along-path mesh), and **MoText** (extruded text). The
scene is three independent node chains in one SOP graph, each emitting actors
into the same scene.

**Time:** ~20 minutes. **End result:** a looping title card.

---

## How curves work here (read this first)

There's no dedicated "curve" object — a curve is just an **ordered point
cloud**: the point order *is* the path, and every point carries an `orient`
quaternion whose local **+Z points along the path tangent**. That single fact
is what makes clone-along-spline free: drop a curve into a cloner and every
clone auto-aligns to the path.

One consequence: **a curve on its own renders as dots** (the engine has no line-
drawing pass yet). That's expected — curves are construction geometry. What you
*see* is the clones, the swept mesh, or the text they drive.

---

## Part 1 — Clone-along-spline: a ring of cubes

1. Open the editor (Modeling workspace). In the SOP graph toolbar, **Add Node…**
   and add, from **Generators**: **Circle** and **Cube**; from **Cloners**:
   **Instance**.

2. Wire the cloner. Instance has **two inputs** — order matters:
   - **Cube → input 0** (`stamp`, the shape to clone).
   - **Circle → input 1** (`template`, the points to clone onto).

   Instance is a *terminal* node — it emits clones directly, so you do **not**
   wire it to Object Output (unlike Copy to Points).

3. Select **Circle** and set:

   | Param | Value |
   |------------|-------|
   | `radius` | `4` |
   | `segments` | `24` |
   | `arc` | `360` (a full, closed ring) |

4. Shrink the clone: select **Cube**, set `size` to `0.5`.

You now have 24 cubes evenly spaced around a 4-unit ring, each oriented to the
ring's tangent (their local +Z follows the circle). That's clone-along-spline.

> The circle's 24 points double as the clone count. To decouple "how smooth the
> path is" from "how many clones," use Resample next.

---

## Part 2 — Resample to control clone spacing

Insert a **Resample** (from **Modifiers**) between the Circle and the Instance
so the clone count is independent of the circle's segment count.

1. **Add Node… → Modifiers → Resample**.

2. Re-wire: **Circle → Resample**, then **Resample → Instance input 1**
   (dropping a new wire on an input replaces the old one).

3. Select **Resample**:

   | Param | Value | Notes |
   |--------------------|---------|-------|
   | `mode` | `count` | even spacing by point count |
   | `count` | `36` | now 36 clones, regardless of the circle |
   | `recompute_frames` | ✔ on | re-derive `orient` so clones stay tangent-aligned |

   Switch `mode` to `length` and set `segment_length` instead if you'd rather
   space clones a fixed distance apart (great when the path length changes).

Bump the Circle's `segments` up to `64` for a rounder path — the clone count
stays at 36 because Resample owns it now.

---

## Part 3 — Sweep a glowing coil

Sweep turns a **profile** cross-section into a tube by sliding it along a
**path**. We'll coil a small circle along a spiral.

1. **Add Node… → Generators → Spiral** (the path) and a second **Circle** (the
   profile). Then **Add Node… → Combiners → Sweep**.

2. Wire Sweep's **two inputs** (path first, like the node's port order):
   - **Spiral → input 0** (`path`, the spine).
   - **Circle (the new one) → input 1** (`profile`, the cross-section).

3. Make Sweep render. It produces geometry (it's not a terminal node like
   Instance), so it needs an **Object Output**. The default graph already has
   one — wire **Sweep → Object Output** into it (drag from Sweep's output port
   to the Object Output's input, or select Sweep and press **O**).

4. Select the **Spiral** (path):

   | Param | Value |
   |--------------|-------|
   | `radius` | `4` |
   | `end_radius` | `4` (constant radius; set lower for a cone coil) |
   | `height` | `3` |
   | `turns` | `4` |
   | `points` | `200` (smooth tube) |

5. Select the **profile Circle**: `radius` `0.15`, `segments` `12`. (A small
   closed circle → a round tube. An open arc profile would make a ribbon.)

6. Select **Sweep**: leave `caps` on. If the tube ever looks inside-out (dark
   where it should be lit), toggle **`flip`** — Sweep auto-detects profile
   winding, but `flip` is the manual override.

You've got a 4-turn coil rising 3 units. The profile circle rode the spiral's
rotation-minimizing frame, so the tube has no pinching or twist.

---

## Part 4 — The 3D text title

1. **Add Node… → Generators → MoText**. Like the coil it outputs a mesh, so it
   needs its **own** Object Output (one Object Output input holds a single
   connection, so the coil and text can't share one). **Add Node… → Output →
   Object Output**, then wire **MoText → (new) Object Output**. Every Object
   Output and every terminal node (like the ring's Instance) contributes its
   actors to the same scene.

2. Select **MoText**:

   | Param | Value | Notes |
   |-------------|----------------------------------------|-------|
   | `text` | `TRACEY` | use `\n` for a line break |
   | `font_file` | `/System/Library/Fonts/Helvetica.ttc` | any .ttf/.otf path |
   | `size` | `2` | cap height in world units |
   | `depth` | `0.3` | extrusion; `0` = flat sheet |
   | `align` | `center` | centers each line on the origin |

   Letters with holes (R, A, O, e) tessellate correctly — the bowls are cut out.

---

## Part 5 — Arrange, light, and animate the spin

The three chains all sit at the origin right now. Space them out with Transform
nodes, then spin the whole thing.

1. **Position the pieces.** Add a **Transform** (Modifiers) after the coil's
   Sweep and after the text's MoText (before each Object Output), and nudge
   them with `translate` — e.g. push the coil behind the text (`translate` z
   `-1`) and lift the text to sit inside the ring. The orbiting cubes can stay
   centered.

2. **Add a light.** In the **Scene Hierarchy** (left), click **+ 💡 Add Light**
   → **Dome** for soft fill (or **Sun** for hard shadows).

3. **Frame the loop.** In the **Playbar**, set the range **start = `1`**,
   **end = `120`** (5 s at 24 fps).

4. **Spin the cube ring.** Insert a **Transform** between Resample and Instance
   (so the whole clone source rotates):
   - **Resample → Transform → Instance input 1**.
   - Enable **AK** (auto-key) in the playbar.
   - At **frame 1**, set the Transform's `rotate_euler_deg` to `[0, 0, 0]`.
   - At **frame 120**, set it to `[0, 360, 0]` (one full turn about Y).
   - Turn **AK** off. Set the playbar **loop** dropdown to **Loop** and press
     **Space** — the ring spins endlessly, seamlessly (360° = 0°).

   > For eased or curved motion, open the **Animation** workspace, switch the
   > bottom panel to **Curves**, and shape the `rotate_euler_deg.y` channel —
   > or set its extrapolation to **Cycle** so it keeps spinning past frame 120.
   > (See the energy-pulse tutorial for the curve-editor walkthrough.)

---

## Part 6 — Render and export

1. Click **PT Preview** in the top toolbar, then switch to the **Render**
   workspace. In the Render panel pick a **Resolution** (`1080p`), and click
   **High Quality Preset** (Samples 4096 / Bounces 8) for the final.

2. Scrub to a clean frame to confirm the look, then press **⌘E** (Export
   Video…). Set frame range `1`–`120`, fps `24`, resolution `1920 × 1080`,
   `128`–`256` samples/frame, `H.264`, and **Export**.

You now have a looping title card: cubes orbiting a glowing coil around your
3D text.

---

## Where to take it next

- **Clone along the coil.** Wire the **Spiral → Resample → Instance** (cube) to
  scatter clones up the helix — clone-along-spline works on any curve, not just
  circles.
- **Per-letter MoGraph.** Feed **MoText → Copy to Points** isn't direct (text is
  a mesh, not points), but you can **Scatter** points on the text mesh and clone
  onto those, or animate the text with the effectors from Phase 1 by treating
  its points as a cloud.
- **Ribbon instead of tube.** Give Sweep an **open** profile (a Line, or a
  Circle with `arc` < 360) → a flat ribbon that banks along the path.
- **Taper the coil.** Set the Spiral's `end_radius` smaller than `radius` for a
  cone-shaped spring.
- **Effectors on the ring.** Drop a **Random** or **Noise Effector** (Phase 1)
  between Resample and the spin Transform to jitter the orbiting cubes.

---

## Quick reference — Phase 2 nodes

**Line / Circle / Spiral** (Generators): emit ordered curve points with
tangent-aligned `orient`. Circle `arc` < 360 → open arc; Spiral has
`radius`/`end_radius` taper, `height`, `turns`.

**Resample** (Modifiers): even spacing by `count` or `segment_length`;
`recompute_frames` keeps `orient` tangent-aligned after resampling.

**Sweep** (Combiners): input 0 = `path`, input 1 = `profile` (cross-section,
authored in the ground/XZ plane). Closed profile → tube, open → ribbon. `caps`
fills the ends; `flip` inverts facing.

**MoText** (Generators): `text` (`\n` for line breaks), `font_file`, `size`,
`depth` (0 = flat), `align` (left/center). Outputs an extruded mesh with cut-out
holes.

**Clone-along-spline**: any curve → **Instance** (input 1) or **Copy to Points**
(input 1). Clones inherit the path orientation via the curve's `orient` (local
+Z follows the tangent).
