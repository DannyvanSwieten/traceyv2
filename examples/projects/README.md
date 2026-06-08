# Example Projects

A curated set of `.tracey` project folders that each showcase a specific
feature slice. Open one in the editor with **File → Open Project** and
the SOP/DOP graphs cook in immediately.

| Folder | Showcases |
| --- | --- |
| `01_geometry_basics` | Generators (cube/sphere/plane), `transform`, `merge`, `object_output`. |
| `02_displaced_sphere` | `attribute_vop` hosting `geo_input` → `noise_fbm` → `displace_along_normal` → `geo_output`. |
| `03_scatter_cubes` | `scatter` + `copy_to_points` cloner workflow. |
| `04_delete_carve` | The new `Delete` SOP carving a bbox-shaped hole from a point grid. |
| `05_sort_color` | `Sort` SOP + a VOP graph driving `Cd` from `ptnum`. |
| `06_particles_wind` | `pop_source` → `pop_gravity` → `pop_drag` → `pop_wind` (with turbulence) → `pop_solver`. |
| `07_particles_attract` | Particles converging on a target via `pop_attract`, with `pop_speed_limit` (soft) and `pop_kill` (bbox). |
| `08_particles_pathtraced` | Same DOP sim as `06`, but `dop_import → instance` (sphere stamp) emits one TLAS instance per particle so the path tracer can actually render them. |

## Regenerating

The files are checked in but produced by the `example_projects` build
target — never edit them by hand. To rebuild after touching the
generator (or after a schema change in the SOP/DOP serializers):

```sh
cmake --build build --target example_projects
./build/examples/example_projects examples/projects
```

The tool round-trips every example through `deserialize → serialize` and
fails loudly if the SOP or DOP graph payload drifts, so a stale example
catches your eye at the build step instead of at editor-open time.

## Layout

Each folder has the standard project shape:

```
<name>/
  <name>.tracey        — v3 root JSON: version + scene + sop_graph + dop_graph + render_settings + timeline
  materials/           — empty placeholder for project-local material library files
```

The `materials/` folder is empty by default; if you save a material from
the editor while one of these projects is open, it lands there.
