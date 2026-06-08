// example_projects — one-shot tool that emits a curated set of
// .tracey project folders to disk. Each example targets a specific
// feature slice (primitives, VOP-driven displacement, scatter +
// instances, the new Delete/Switch/Sort SOPs, the new pop_drag /
// pop_wind / pop_attract / pop_kill DOPs) so the editor can showcase
// — and so we can UX-test — features end-to-end with one click of
// File → Open Project.
//
// The build target also serves as a smoke test for the SOP + DOP
// serializers: any drift in the JSON schema fails to compile here
// before the editor ever loads a stale file.
//
// Usage:
//   ./example_projects [output_dir]
// Defaults to `examples/projects/` relative to the binary's run dir.
//
// On-disk layout per project:
//   <name>/
//     <name>.tracey          — the project file (v3 root JSON)
//     materials/             — empty placeholder for project-local materials
//                              the user might author after opening

#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"  // also exposes registerBuiltinSops()

#include "dops/register_builtins.hpp"
#include "dops/serialization.hpp"
#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"

#include "vops/register_builtins.hpp"
#include "vops/serialization.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_node.hpp"
#include "vops/vop_registry.hpp"

#include "sops/nodes/attribute_vop_sop.hpp"

#include "scene/camera.hpp"

#include "core/types.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // ── Small helpers so each project builder reads top-to-bottom ──

    using namespace tracey;
    using namespace tracey::sops;

    // Drop a SOP of the given kind, optionally positioned for the canvas,
    // and return its uid. The wire-up helper below pairs neatly with this.
    size_t addSop(SopGraph &g, const std::string &kind, float x, float y,
                  const std::function<void(SopNode &)> &setup = {})
    {
        auto node = SopRegistry::instance().create(kind, g.nextUid());
        if (!node) throw std::runtime_error("unknown SOP kind: " + kind);
        node->setPos(x, y);
        if (setup) setup(*node);
        const size_t uid = node->uid();
        g.addNode(std::move(node));
        return uid;
    }

    void wire(SopGraph &g, size_t fromUid, size_t fromPort, size_t toUid, size_t toPort = 0)
    {
        g.createConnection(fromUid, fromPort, toUid, toPort);
    }

    size_t addDop(dops::DopGraph &g, const std::string &kind, float x, float y,
                  const std::function<void(dops::DopNode &)> &setup = {})
    {
        auto node = dops::DopRegistry::instance().create(kind, g.nextUid());
        if (!node) throw std::runtime_error("unknown DOP kind: " + kind);
        node->setPos(x, y);
        if (setup) setup(*node);
        const size_t uid = node->uid();
        g.addNode(std::move(node));
        return uid;
    }

    void wireDop(dops::DopGraph &g, size_t fromUid, size_t toUid)
    {
        g.createConnection(fromUid, 0, toUid, 0);
    }

    // VOP graph builder. The unified geo_input/geo_output approach means
    // every VOP graph starts with one input + one output; the per-port
    // wiring is what makes each example different.
    size_t addVop(vops::VopGraph &g, const std::string &kind, float x, float y,
                  const std::function<void(vops::VopNode &)> &setup = {})
    {
        auto node = vops::VopRegistry::instance().create(kind, g.nextUid());
        if (!node) throw std::runtime_error("unknown VOP kind: " + kind);
        node->setPos(x, y);
        if (setup) setup(*node);
        const size_t uid = node->uid();
        g.addNode(std::move(node));
        return uid;
    }

    void wireVop(vops::VopGraph &g, size_t fromUid, size_t fromPort,
                 size_t toUid, size_t toPort)
    {
        g.createConnection(fromUid, fromPort, toUid, toPort);
    }

    // ── Camera presets ──
    // A small orbit camera looking at the origin from a comfortable
    // 3/4-front angle. Every example uses this unless it wants a
    // closer-up or top-down view; tweaking once here keeps the
    // showcase reads consistent.
    Camera defaultCamera()
    {
        Camera c;
        c.setPosition(Vec3(4.0f, 3.0f, 6.0f));
        // Look at origin, then derive rotation. For simplicity here we
        // skip computing the quaternion and let the editor fix the
        // camera up on first frame; the position alone is the
        // load-time hint that matters.
        c.setFov(45.0f);
        c.setNearPlane(0.1f);
        c.setFarPlane(1000.0f);
        c.setAspectRatio(16.0f / 9.0f);
        return c;
    }

    // Assemble a v3 .tracey root JSON from the parts each builder
    // produces. The schema mirrors editor_server.cpp's save_scene
    // handler — if either drifts, this file fails to round-trip
    // through load_scene on next open.
    //
    // We assemble the JSON as a raw string instead of pulling in
    // nlohmann::json (which would require routing the tinygltf
    // include path into this target just for one writer call).
    // The inner SOP/DOP/VOP graphs already come out of their own
    // serializers as JSON strings — we just need to splice them in.
    std::string makeProjectJson(const SopGraph &sop, const dops::DopGraph &dop,
                                const Camera &camera, int frameEnd = 240,
                                double fps = 24.0,
                                bool showPoints = false)
    {
        const std::string sopJson = serializeSopGraphPretty(sop);
        const std::string dopJson = dops::serializeDopGraphPretty(dop);

        std::ostringstream os;
        // Always emit at full precision — the editor parses floats
        // verbatim, so trailing-digit drift here doesn't matter
        // visually, but stable output makes the checked-in files
        // diff cleanly across regenerations.
        os.precision(9);
        os << "{\n";
        os << "  \"version\": 3,\n";
        os << "  \"scene\": {\n";
        os << "    \"camera\": {\n";
        os << "      \"position\": {\"x\": " << camera.position().x
           << ", \"y\": " << camera.position().y
           << ", \"z\": " << camera.position().z << "},\n";
        os << "      \"rotation\": {\"w\": " << camera.rotation().w
           << ", \"x\": " << camera.rotation().x
           << ", \"y\": " << camera.rotation().y
           << ", \"z\": " << camera.rotation().z << "},\n";
        os << "      \"fov\": " << camera.fov() << ",\n";
        os << "      \"near_plane\": " << camera.nearPlane() << ",\n";
        os << "      \"far_plane\": " << camera.farPlane() << ",\n";
        os << "      \"aspect_ratio\": " << camera.aspectRatio() << "\n";
        os << "    },\n";
        os << "    \"actors\": []\n";
        os << "  },\n";
        os << "  \"sop_graph\": " << sopJson << ",\n";
        os << "  \"dop_graph\": " << dopJson << ",\n";
        os << "  \"render_settings\": {\n";
        os << "    \"max_samples\": 64,\n";
        os << "    \"max_bounces\": 4,\n";
        os << "    \"show_points\": " << (showPoints ? "true" : "false") << ",\n";
        os << "    \"show_edges\": false\n";
        os << "  },\n";
        os << "  \"timeline\": {\n";
        os << "    \"fps\": " << fps << ",\n";
        os << "    \"frame_start\": 1,\n";
        os << "    \"frame_end\": " << frameEnd << ",\n";
        os << "    \"current_time\": 0.0,\n";
        os << "    \"loop\": 1\n";
        os << "  }\n";
        os << "}\n";
        return os.str();
    }

    // ── Project builders ──────────────────────────────────────────────────
    // Each builder returns a fully assembled root JSON; the main()
    // dispatcher writes it to disk under <out>/<name>/<name>.tracey.

    // 01 — Geometry basics: three primitives positioned around the origin,
    // merged into one output. Drag/drop test for the canvas, smoke test
    // for primitive_cube / primitive_sphere / primitive_plane / transform /
    // merge / object_output.
    std::string buildGeometryBasics()
    {
        SopGraph sop(0);
        // SOP graphs read top-to-bottom (Y grows downstream); each
        // generator branch sits in its own column and the chain converges
        // through merges into the final object_output.
        const size_t cube   = addSop(sop, "primitive_cube",   100, 100,
            [](SopNode &n){ n.setParamFloat("size", 1.2f); });
        const size_t cubeX  = addSop(sop, "transform",        100, 220,
            [](SopNode &n){ n.setParamVec3("translate", Vec3(-2.0f, 0.0f, 0.0f)); });
        const size_t sphere = addSop(sop, "primitive_sphere", 280, 100,
            [](SopNode &n){ n.setParamFloat("radius", 0.8f); });
        const size_t plane  = addSop(sop, "primitive_plane",  460, 100,
            [](SopNode &n){
                n.setParamFloat("width", 6.0f);
                n.setParamFloat("depth", 6.0f);
            });
        const size_t planeY = addSop(sop, "transform",        460, 220,
            [](SopNode &n){ n.setParamVec3("translate", Vec3(0.0f, -1.0f, 0.0f)); });
        const size_t m1     = addSop(sop, "merge",            190, 380);
        const size_t m2     = addSop(sop, "merge",            320, 520);
        const size_t out    = addSop(sop, "object_output",    320, 660,
            [](SopNode &n){ n.setParamString("name", "geometry_basics"); });

        wire(sop, cube,   0, cubeX, 0);
        wire(sop, plane,  0, planeY, 0);
        wire(sop, cubeX,  0, m1, 0);
        wire(sop, sphere, 0, m1, 1);
        wire(sop, m1,     0, m2, 0);
        wire(sop, planeY, 0, m2, 1);
        wire(sop, m2,     0, out, 0);

        return makeProjectJson(sop, dops::DopGraph{0}, defaultCamera());
    }

    // 02 — Displaced sphere: primitive_sphere into an attribute_vop whose
    // child VOP graph perturbs P along N by fbm-noise. Showcases the
    // unified geo_input → geo_output terminals, the noise VOPs, and the
    // displace_along_normal node in one shot.
    std::string buildDisplacedSphere()
    {
        SopGraph sop(0);
        const size_t sphere = addSop(sop, "primitive_sphere", 200, 100,
            [](SopNode &n){
                n.setParamFloat("radius", 1.0f);
                n.setParamInt("segments", 64);
                n.setParamInt("rings", 32);
            });
        const size_t normals = addSop(sop, "normal", 200, 220,
            [](SopNode &n){ n.setParamString("mode", "smooth"); });
        const size_t avop    = addSop(sop, "attribute_vop", 200, 340);
        const size_t normals2 = addSop(sop, "normal", 200, 460,
            [](SopNode &n){ n.setParamString("mode", "smooth"); });
        const size_t out     = addSop(sop, "object_output", 200, 580,
            [](SopNode &n){ n.setParamString("name", "displaced_sphere"); });

        wire(sop, sphere, 0, normals, 0);
        wire(sop, normals, 0, avop, 0);
        wire(sop, avop,   0, normals2, 0);  // re-smooth normals after displacement
        wire(sop, normals2, 0, out, 0);

        // Author the inner VOP graph: geo_input.P → fbm → displace_along_normal
        // → geo_output.P. Port indices match materialInputPorts() /
        // materialOutputPorts() in src/vops/nodes/geo_io_vops.cpp.
        auto vopOwned = std::make_unique<vops::VopGraph>(0);
        auto &vop = *vopOwned;
        const size_t gin  = addVop(vop, "geo_input",  80,  120);
        const size_t fbm  = addVop(vop, "noise_fbm",  280, 120,
            [](vops::VopNode &n){
                n.setParamFloat("frequency", 1.5f);
                n.setParamFloat("amplitude", 0.15f);
                n.setParamInt("octaves", 4);
                n.setParamFloat("lacunarity", 2.0f);
                n.setParamFloat("gain", 0.5f);
                n.setParamInt("seed", 7);
            });
        const size_t disp = addVop(vop, "displace_along_normal", 500, 120);
        const size_t gout = addVop(vop, "geo_output", 720, 120);

        // geo_input ports: 0=P, 1=N. geo_output ports: 0=P.
        wireVop(vop, gin, 0, fbm,  0);
        wireVop(vop, gin, 0, disp, 0);  // P
        wireVop(vop, gin, 1, disp, 1);  // N
        wireVop(vop, fbm, 0, disp, 2);  // amount
        wireVop(vop, disp, 0, gout, 0); // → MaterialOutput.Albedo... wait this is geo not material
        // Above is geo_output.P (port 0), so the wire is correct.

        setAttributeVopGraph(sop.findNode(avop), std::move(vopOwned));

        return makeProjectJson(sop, dops::DopGraph{0}, defaultCamera());
    }

    // 03 — Scatter cubes: points_grid → scatter → copy_to_points with a
    // small cube as the template. Classic cloners showcase + a chance to
    // exercise instance scaling.
    std::string buildScatterCubes()
    {
        SopGraph sop(0);
        // Two source branches (template = scattered plane, stamp = cube)
        // converge into copy_to_points → object_output. Top-to-bottom.
        const size_t plane  = addSop(sop, "primitive_plane", 100, 100,
            [](SopNode &n){
                n.setParamFloat("width", 6.0f);
                n.setParamFloat("depth", 6.0f);
                n.setParamInt("cols", 8);
                n.setParamInt("rows", 8);
            });
        const size_t scat   = addSop(sop, "scatter",         100, 220,
            [](SopNode &n){
                n.setParamInt("count", 200);
                n.setParamInt("seed", 13);
            });
        const size_t cube   = addSop(sop, "primitive_cube",  300, 100,
            [](SopNode &n){ n.setParamFloat("size", 0.18f); });
        const size_t ctp    = addSop(sop, "copy_to_points",  200, 380);
        const size_t out    = addSop(sop, "object_output",   200, 500,
            [](SopNode &n){ n.setParamString("name", "scatter_cubes"); });

        wire(sop, plane, 0, scat, 0);
        // copy_to_points port 0 = STAMP (geometry to clone), port 1 =
        // TEMPLATE (point cloud to clone onto). The earlier wiring had
        // these swapped, which silently produced a "200 scatter points
        // cloned 8 times" output — point data only, no triangles —
        // which is why the rasterizer's triangle pipeline rendered
        // nothing for this example and only the points overlay made
        // anything visible.
        wire(sop, cube, 0, ctp, 0);  // cube  → stamp
        wire(sop, scat, 0, ctp, 1);  // scatter → template
        wire(sop, ctp,  0, out, 0);

        return makeProjectJson(sop, dops::DopGraph{0}, defaultCamera());
    }

    // 04 — Delete carve: a dense points_grid with a bbox-shaped hole
    // through the middle. Best UX showcase for the new Delete SOP and
    // for the Bound node (drop one after to visualise the hole's footprint).
    std::string buildDeleteCarve()
    {
        SopGraph sop(0);
        const size_t grid = addSop(sop, "points_grid", 100, 100,
            [](SopNode &n){
                n.setParamInt  ("count_x",   40);
                n.setParamInt  ("count_y",    1);
                n.setParamInt  ("count_z",   40);
                n.setParamFloat("spacing_x", 0.1f);
                n.setParamFloat("spacing_y", 0.1f);
                n.setParamFloat("spacing_z", 0.1f);
            });
        const size_t del = addSop(sop, "delete", 100, 220,
            [](SopNode &n){
                n.setParamString("mode", "bbox");
                n.setParamVec3("bbox_min", Vec3(-0.8f, -0.8f, -0.8f));
                n.setParamVec3("bbox_max", Vec3( 0.8f,  0.8f,  0.8f));
                // invert=true means "delete points INSIDE the bbox"
                // (default mode "bbox" keeps inside; invert flips it).
                n.setParamBool("invert", true);
            });
        const size_t out = addSop(sop, "object_output", 100, 340,
            [](SopNode &n){ n.setParamString("name", "delete_carve"); });

        wire(sop, grid, 0, del, 0);
        wire(sop, del,  0, out, 0);

        // points-only output — turn the points overlay on so the dots
        // actually show.
        return makeProjectJson(sop, dops::DopGraph{0}, defaultCamera(),
                               /*frameEnd=*/240, /*fps=*/24.0,
                               /*showPoints=*/true);
    }

    // 05 — Sort + color: a points_grid sorted by X position, then coloured
    // by point index via an attribute_vop reading ptnum from geo_input.
    // Showcases sort_sop + the ptnum read port + the divide/multiply VOP
    // chain into geo_output.Cd.
    std::string buildSortColor()
    {
        SopGraph sop(0);
        const size_t grid = addSop(sop, "points_grid", 100, 100,
            [](SopNode &n){
                n.setParamInt  ("count_x",   30);
                n.setParamInt  ("count_y",    1);
                n.setParamInt  ("count_z",   30);
                n.setParamFloat("spacing_x", 0.15f);
                n.setParamFloat("spacing_y", 0.15f);
                n.setParamFloat("spacing_z", 0.15f);
            });
        const size_t sort = addSop(sop, "sort", 100, 220,
            [](SopNode &n){
                n.setParamString("mode", "axis");
                n.setParamString("axis", "+X");
            });
        const size_t avop = addSop(sop, "attribute_vop", 100, 340);
        const size_t out  = addSop(sop, "object_output", 100, 460,
            [](SopNode &n){ n.setParamString("name", "sort_color"); });

        wire(sop, grid, 0, sort, 0);
        wire(sop, sort, 0, avop, 0);
        wire(sop, avop, 0, out, 0);

        // VOP: Cd = vec3(t, 1-t, 0)  where t = ptnum / N.
        // Reads as a horizontal green-to-red gradient because we sorted
        // by +X first — proves the sort actually reordered the points
        // (without it the gradient would shuffle randomly across the grid).
        auto vopOwned = std::make_unique<vops::VopGraph>(0);
        auto &vop = *vopOwned;
        const size_t gin  = addVop(vop, "geo_input", 80, 120);
        // Normalize ptnum into [0,1] via dividing by total point count.
        const size_t kN   = addVop(vop, "constant_float", 280, 60,
            [](vops::VopNode &n){ n.setParamFloat("value", 1.0f / 900.0f); });
        const size_t mul  = addVop(vop, "multiply", 460, 100);
        // Build a vec3(r, g, b) gradient using fit + make_vec3.
        const size_t kOne = addVop(vop, "constant_float", 280, 220,
            [](vops::VopNode &n){ n.setParamFloat("value", 1.0f); });
        const size_t inv  = addVop(vop, "subtract", 640, 100);  // 1 - t
        const size_t k01  = addVop(vop, "constant_float", 460, 260,
            [](vops::VopNode &n){ n.setParamFloat("value", 0.0f); });
        const size_t mk   = addVop(vop, "make_vec3", 820, 140);
        const size_t gout = addVop(vop, "geo_output", 1000, 140);

        // ptnum is geo_input output port 10 (see materialInputPorts()).
        // Wait — that's for the material graph. For VOP geo graph, ptnum
        // is the LAST port of geo_input (index 10): P,N,Cd,uv,v,force,
        // Alpha,pscale,age,life,ptnum.
        const size_t kGioPtnum = 10;
        const size_t kGioCd    = 2;
        wireVop(vop, gin, kGioPtnum, mul, 0);  // ptnum
        wireVop(vop, kN,  0, mul, 1);          // * (1/N) → t in [0,1]
        wireVop(vop, kOne, 0, inv, 0);         // 1
        wireVop(vop, mul,  0, inv, 1);         //  - t
        wireVop(vop, mul,  0, mk, 0);  // r = t
        wireVop(vop, inv,  0, mk, 1);  // g = 1 - t
        wireVop(vop, k01,  0, mk, 2);  // b = 0
        wireVop(vop, mk,   0, gout, kGioCd);

        setAttributeVopGraph(sop.findNode(avop), std::move(vopOwned));

        // points-only output — show_points so the gradient is visible.
        return makeProjectJson(sop, dops::DopGraph{0}, defaultCamera(),
                               /*frameEnd=*/240, /*fps=*/24.0,
                               /*showPoints=*/true);
    }

    // 06 — Particles with drag + wind: pop_source emits a stream, gravity
    // pulls down, wind blows them sideways with turbulence, drag stops
    // runaway. Tests the full new force pipeline in one go.
    std::string buildParticlesWind()
    {
        // DOP graph: source → gravity → drag → wind → solver.
        dops::DopGraph dop(0);
        const size_t src   = addDop(dop, "pop_source", 100, 100,
            [](dops::DopNode &n){
                n.setParamFloat("rate", 60.0f);
                n.setParamFloat("lifetime", 6.0f);
                n.setParamVec3("origin", Vec3(0.0f, 2.0f, 0.0f));
                n.setParamVec3("initial_v", Vec3(0.0f, 1.0f, 0.0f));
            });
        const size_t grav  = addDop(dop, "pop_gravity", 280, 100,
            [](dops::DopNode &n){
                n.setParamVec3("gravity", Vec3(0.0f, -3.0f, 0.0f));
            });
        const size_t drag  = addDop(dop, "pop_drag", 460, 100,
            [](dops::DopNode &n){ n.setParamFloat("drag", 0.5f); });
        const size_t wind  = addDop(dop, "pop_wind", 640, 100,
            [](dops::DopNode &n){
                n.setParamVec3("direction", Vec3(1.0f, 0.0f, 0.3f));
                n.setParamFloat("speed", 2.0f);
                n.setParamFloat("turbulence", 1.5f);
                n.setParamFloat("turbulence_freq", 0.8f);
                n.setParamInt("seed", 23);
            });
        const size_t solv  = addDop(dop, "pop_solver", 820, 100);
        wireDop(dop, src,  grav);
        wireDop(dop, grav, drag);
        wireDop(dop, drag, wind);
        wireDop(dop, wind, solv);

        // SOP graph: dop_import → object_output (top-to-bottom).
        SopGraph sop(0);
        const size_t imp = addSop(sop, "dop_import", 100, 100);
        const size_t out = addSop(sop, "object_output", 100, 220,
            [](SopNode &n){ n.setParamString("name", "particles_wind"); });
        wire(sop, imp, 0, out, 0);

        // Tighter framerate so playback feels smooth on the wind sim.
        // Particles are point-only, so the points overlay needs to be on
        // for anything to be visible at all.
        return makeProjectJson(sop, dop, defaultCamera(), /*frameEnd=*/360,
                               /*fps=*/24.0, /*showPoints=*/true);
    }

    // 07 — Particles attracted to origin with a soft speed limit and
    // bbox-kill so the swarm doesn't drift off-screen. Targets pop_attract
    // and pop_speed_limit specifically; pop_kill prunes anything that
    // escapes the play volume.
    std::string buildParticlesAttract()
    {
        dops::DopGraph dop(0);
        const size_t src    = addDop(dop, "pop_source", 100, 100,
            [](dops::DopNode &n){
                n.setParamFloat("rate", 40.0f);
                n.setParamFloat("lifetime", 10.0f);
                n.setParamVec3("origin", Vec3(5.0f, 2.0f, 5.0f));
                n.setParamVec3("initial_v", Vec3(-2.0f, 0.0f, -2.0f));
            });
        const size_t attr   = addDop(dop, "pop_attract", 280, 100,
            [](dops::DopNode &n){
                n.setParamVec3("target",   Vec3(0.0f, 0.0f, 0.0f));
                n.setParamFloat("strength", 2.0f);
                n.setParamFloat("falloff",  4.0f);
            });
        const size_t drag   = addDop(dop, "pop_drag", 460, 100,
            [](dops::DopNode &n){ n.setParamFloat("drag", 0.3f); });
        const size_t solv   = addDop(dop, "pop_solver", 640, 100);
        const size_t limit  = addDop(dop, "pop_speed_limit", 820, 100,
            [](dops::DopNode &n){
                n.setParamString("mode", "soft");
                n.setParamFloat("max_speed", 4.0f);
                n.setParamFloat("soft_rate", 8.0f);
            });
        const size_t kill   = addDop(dop, "pop_kill", 1000, 100,
            [](dops::DopNode &n){
                n.setParamString("mode", "bbox");
                n.setParamVec3("bbox_min", Vec3(-8.0f, -4.0f, -8.0f));
                n.setParamVec3("bbox_max", Vec3( 8.0f,  8.0f,  8.0f));
            });
        wireDop(dop, src,  attr);
        wireDop(dop, attr, drag);
        wireDop(dop, drag, solv);
        wireDop(dop, solv, limit);
        wireDop(dop, limit, kill);

        SopGraph sop(0);
        const size_t imp = addSop(sop, "dop_import", 100, 100);
        const size_t out = addSop(sop, "object_output", 100, 220,
            [](SopNode &n){ n.setParamString("name", "particles_attract"); });
        wire(sop, imp, 0, out, 0);

        // Particles → points overlay on.
        return makeProjectJson(sop, dop, defaultCamera(), /*frameEnd=*/360,
                               /*fps=*/24.0, /*showPoints=*/true);
    }

    // 08 — Path-traced particles. Same DOP sim shape as 06_particles_wind,
    // but the SOP graph instances a small sphere onto every particle so
    // the path tracer sees actual geometry to raytrace. Each particle
    // becomes a TLAS instance pointing at one shared sphere BLAS — memory
    // is O(sphere + N transforms), so even 10K+ particles stay cheap.
    // Per-point Cd → per-instance albedo tint, pscale → per-instance
    // radius (we don't drive these in this example but the wiring is
    // there for the user to extend via attribute_vop).
    std::string buildParticlesPathtraced()
    {
        // Same DOP shape as 06 — emit, drag, wind. Slightly slower wind
        // so the particles linger in the camera frame longer.
        dops::DopGraph dop(0);
        const size_t src   = addDop(dop, "pop_source", 100, 100,
            [](dops::DopNode &n){
                n.setParamFloat("rate", 30.0f);
                n.setParamFloat("lifetime", 4.0f);
                n.setParamVec3("origin", Vec3(0.0f, 0.5f, 0.0f));
                n.setParamVec3("initial_v", Vec3(0.0f, 2.0f, 0.0f));
                n.setParamFloat("pos_jitter", 0.1f);
            });
        const size_t grav  = addDop(dop, "pop_gravity", 280, 100,
            [](dops::DopNode &n){
                n.setParamVec3("gravity", Vec3(0.0f, -2.0f, 0.0f));
            });
        const size_t drag  = addDop(dop, "pop_drag", 460, 100,
            [](dops::DopNode &n){ n.setParamFloat("drag", 0.4f); });
        const size_t wind  = addDop(dop, "pop_wind", 640, 100,
            [](dops::DopNode &n){
                n.setParamVec3("direction", Vec3(1.0f, 0.0f, 0.0f));
                n.setParamFloat("speed", 1.0f);
                n.setParamFloat("turbulence", 1.2f);
                n.setParamFloat("turbulence_freq", 0.6f);
                n.setParamInt("seed", 31);
            });
        const size_t solv  = addDop(dop, "pop_solver", 820, 100);
        wireDop(dop, src,  grav);
        wireDop(dop, grav, drag);
        wireDop(dop, drag, wind);
        wireDop(dop, wind, solv);

        // SOP graph: a small sphere as the stamp, dop_import as the
        // template, `instance` ties them together and emits one TLAS
        // entry per particle. `instance` is a terminal — no output port,
        // emits straight into the scene.
        SopGraph sop(0);
        // Two branches (sphere stamp + dop_import template) converge into
        // the terminal `instance` SOP. Top-to-bottom.
        const size_t sphere = addSop(sop, "primitive_sphere", 100, 100,
            [](SopNode &n){
                n.setParamFloat("radius", 0.06f);
                n.setParamInt("segments", 12);
                n.setParamInt("rings",    8);
            });
        const size_t imp = addSop(sop, "dop_import", 300, 100);
        const size_t inst = addSop(sop, "instance",    200, 280,
            [](SopNode &n){
                n.setParamString("name", "particles_pathtraced");
                // Particles don't carry meaningful N today (the sim
                // doesn't author one), so leaving orient_to_normal off
                // keeps every sphere axis-aligned — that's fine for a
                // round stamp where orientation is invisible anyway.
                n.setParamBool("orient_to_normal", false);
            });
        // instance: input 0 = stamp (sphere), input 1 = template (points).
        wire(sop, sphere, 0, inst, 0);
        wire(sop, imp,    0, inst, 1);
        // instance is terminal — no further wiring.

        // Path-traced output, so the points-overlay isn't needed (and
        // would just clutter the view with sprite dots on top of the
        // raytraced spheres).
        return makeProjectJson(sop, dop, defaultCamera(), /*frameEnd=*/360,
                               /*fps=*/24.0, /*showPoints=*/false);
    }

    // ── Driver ─────────────────────────────────────────────────────────────

    struct Example
    {
        const char *folder;       // <out>/<folder>/<folder>.tracey
        const char *description;  // printed on stdout for traceability
        std::function<std::string()> build;
    };

    void writeProject(const fs::path &outDir, const Example &ex)
    {
        const fs::path projDir = outDir / ex.folder;
        fs::create_directories(projDir / "materials");

        const std::string root = ex.build();
        const fs::path file = projDir / (std::string(ex.folder) + ".tracey");
        std::ofstream out(file);
        if (!out) throw std::runtime_error("cannot open " + file.string());
        out << root;
        std::cout << "  " << file.string() << "  — " << ex.description << "\n";
    }

    // Round-trip every example's SOP + DOP graph through the serializers.
    // Catches schema drift before the editor ever opens the file — if any
    // example registers a node kind that fails to deserialize, this fails
    // here with a clear error rather than silently producing a broken
    // .tracey. We don't compare the outer wrapper JSON because the
    // editor's load_scene reads it directly; the inner graph payloads
    // are what go through (de)serializeXxxGraph and are most prone to
    // breakage when a node's params change.
    void verifyRoundtrip(const Example &ex)
    {
        const std::string root = ex.build();
        // Pull the inner sop_graph / dop_graph payloads out by string
        // matching — we don't have a JSON parser available in this
        // target and the format here is generated by us, so the
        // substring locator is safe.
        auto extract = [&](const std::string &key) -> std::string {
            const std::string anchor = "\"" + key + "\": ";
            const size_t at = root.find(anchor);
            if (at == std::string::npos) return {};
            size_t i = at + anchor.size();
            if (i >= root.size() || root[i] != '{') return {};
            // Walk forward over balanced braces, accounting for strings
            // so escaped braces inside e.g. param value strings don't
            // throw the counter off.
            int depth = 0; bool inStr = false; bool esc = false;
            const size_t start = i;
            for (; i < root.size(); ++i) {
                char c = root[i];
                if (inStr) {
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) { ++i; break; }
                }
            }
            return root.substr(start, i - start);
        };

        const std::string sopJson = extract("sop_graph");
        const std::string dopJson = extract("dop_graph");
        if (sopJson.empty() || dopJson.empty())
            throw std::runtime_error("verify: failed to extract sop/dop payload");

        auto sopRT = sops::deserializeSopGraph(sopJson);
        auto dopRT = dops::deserializeDopGraph(dopJson);
        if (!sopRT) throw std::runtime_error("verify: SOP graph failed to deserialize");
        if (!dopRT) throw std::runtime_error("verify: DOP graph failed to deserialize");
        // Re-serialize and compare. Byte-stable JSON proves both nodes +
        // connections + params round-trip without loss.
        const std::string sopRoundtrip = sops::serializeSopGraphPretty(*sopRT);
        const std::string dopRoundtrip = dops::serializeDopGraphPretty(*dopRT);
        if (sopRoundtrip != sopJson)
            throw std::runtime_error("verify: SOP graph round-trip drifted");
        if (dopRoundtrip != dopJson)
            throw std::runtime_error("verify: DOP graph round-trip drifted");
    }
}  // namespace

int main(int argc, char **argv)
{
    // The shared registries need their built-in catalogs populated
    // before any node is created. Idempotent on second call so an
    // accidental double-register in a future entry point is harmless.
    tracey::sops::registerBuiltinSops();
    tracey::vops::registerBuiltinVops();
    tracey::dops::registerBuiltinDops();

    fs::path outDir = (argc >= 2) ? fs::path(argv[1])
                                  : fs::path("examples/projects");
    fs::create_directories(outDir);

    const std::array<Example, 8> examples = {{
        {"01_geometry_basics",
         "cube + sphere + plane, transformed and merged",
         buildGeometryBasics},
        {"02_displaced_sphere",
         "sphere displaced by fBm noise via attribute_vop",
         buildDisplacedSphere},
        {"03_scatter_cubes",
         "plane → scatter → copy_to_points with cube template",
         buildScatterCubes},
        {"04_delete_carve",
         "points_grid with a bbox-shaped hole carved out (Delete SOP)",
         buildDeleteCarve},
        {"05_sort_color",
         "sorted point grid coloured by point index via VOP",
         buildSortColor},
        {"06_particles_wind",
         "particle stream under gravity + drag + turbulent wind",
         buildParticlesWind},
        {"07_particles_attract",
         "particles attracted to origin with speed limit + bbox kill",
         buildParticlesAttract},
        {"08_particles_pathtraced",
         "particles instanced as small spheres so the PT can render them",
         buildParticlesPathtraced},
    }};

    std::cout << "Writing example projects to " << outDir.string() << ":\n";
    for (const auto &ex : examples)
    {
        try {
            verifyRoundtrip(ex);  // schema check; throws on drift
            writeProject(outDir, ex);
        }
        catch (const std::exception &e) {
            std::cerr << "FAIL " << ex.folder << ": " << e.what() << "\n";
            return 1;
        }
    }
    std::cout << "Done (" << examples.size() << " projects, all round-tripped clean).\n";
    return 0;
}
