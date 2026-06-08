// pop_source — particle emitter.
//
// Each substep emits `rate * dt` particles at `origin` with `initial_v`,
// optionally jittered within a sphere of radius `pos_jitter`. New points
// get a fresh `id` from a monotonically-incrementing detail counter so
// downstream nodes (and renderers) can track particles across frames even
// after the solver compacts the array.
//
// Fractional emissions are accumulated in a per-source detail attribute
// `__src_<uid>_carry` so rate=10/s with dt=1/24s emits 0 + 0 + ... + 1 +
// 0 + ... rather than dropping the fractions. This keeps the visible
// emission rate honest across any substep / fps combination.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../eval_context.hpp"
#include "../sim_state.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace tracey
{
    namespace dops
    {
        namespace
        {
            // 32-bit hash mixing four ints — used to seed the per-emission
            // jitter RNG so emission across (frame, substep, source_uid,
            // emission_index) is decorrelated yet deterministic. Re-running
            // the cook with the same params produces the same particles.
            uint32_t hash4i(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
            {
                uint32_t h = a * 2654435761u
                           ^ b * 2246822519u
                           ^ c *  374761393u
                           ^ d * 3266489917u;
                h ^= h >> 13; h *= 0x85ebca6bu;
                h ^= h >> 16; h *= 0xc2b2ae35u;
                h ^= h >> 13;
                return h;
            }
            // Uniform float in [0, 1) from a single hash.
            float hashToFloat(uint32_t h)
            {
                return static_cast<float>(h & 0x00ffffffu) /
                       static_cast<float>(0x01000000u);
            }

            // Ensure `name` exists on the point table with type T; create
            // (zero-filled to the table's current size) if missing.
            template <typename T>
            Attribute<T> *ensurePointAttr(Geometry &geo, const std::string &name, T def)
            {
                if (auto *a = geo.points().get<T>(name)) return a;
                return geo.points().add<T>(name, def);
            }
            template <typename T>
            Attribute<T> *ensureDetailAttr(Geometry &geo, const std::string &name, T def)
            {
                if (auto *a = geo.detail().get<T>(name)) return a;
                return geo.detail().add<T>(name, def);
            }
        }

        class PopSourceDop : public DopNode
        {
        public:
            explicit PopSourceDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeFloat("rate",       50.0f));
                declareParam(Parameter::makeFloat("lifetime",    2.0f));
                declareParam(Parameter::makeVec3 ("origin",      Vec3(0.0f)));
                declareParam(Parameter::makeVec3 ("initial_v",   Vec3(0.0f, 1.0f, 0.0f)));
                declareParam(Parameter::makeFloat("pos_jitter",  0.0f));
                declareParam(Parameter::makeInt  ("seed",        0));
                // emit_mode = 0 → point (legacy), 1 → geometry (sample from
                // a source SOP's points). The ParamSpec on the registry
                // side ships an `options` array so the inspector renders
                // a named dropdown instead of a raw int.
                declareParam(Parameter::makeInt  ("emit_mode",     0));
                declareParam(Parameter::makeInt  ("source_sop_uid", 0));
                // When emit_mode = geometry, override initial_v with the
                // source point's N × normal_speed. Setting use_normal=false
                // falls back to the constant initial_v param.
                declareParam(Parameter::makeBool ("use_normal",    true));
                declareParam(Parameter::makeFloat("normal_speed",  1.0f));
                declareParam(Parameter::makeBool ("inherit_cd",    true));
            }
            std::string kind() const override { return "pop_source"; }
            InputsAndOutputs ports() const override { return InputsAndOutputs{}; }

            void prepare(SimState &state) const override
            {
                Geometry &g = state.geometry;
                // Standard particle point attributes. P already exists (the
                // Geometry constructor adds it). The rest get created on
                // first prepare(); subsequent prepares are no-ops.
                ensurePointAttr<Vec3> (g, "v",     Vec3(0.0f));
                ensurePointAttr<float>(g, "age",   0.0f);
                ensurePointAttr<float>(g, "life",  1.0f);
                ensurePointAttr<int>  (g, "id",    0);
                ensurePointAttr<Vec3> (g, "force", Vec3(0.0f));
                // Cd materialised eagerly when geometry-source mode might
                // inherit colour from the source's Cd attribute. Cheaper
                // than adding it lazily on the first spawn (which would
                // need to back-fill all pre-existing points with white).
                if (paramInt("emit_mode", 0) == 1 && paramBool("inherit_cd", true))
                    ensurePointAttr<Vec3>(g, "Cd", Vec3(1.0f));
                // Shared monotonic id counter — multiple pop_sources can
                // coexist; they all draw from the same pool so ids stay
                // unique across the geometry.
                ensureDetailAttr<int>(g, "__pop_next_id", 0);
                // Per-source fractional-emission carry. Name keyed by uid
                // so multiple sources don't collide.
                ensureDetailAttr<float>(g, "__src_" + std::to_string(uid()) + "_carry", 0.0f);
                // Per-source rotating index into the source SOP's point
                // list (geometry mode). Survives substeps so emissions
                // cycle through the source rather than restarting at 0
                // every frame.
                ensureDetailAttr<int>(g, "__src_" + std::to_string(uid()) + "_idx", 0);
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                const SimHeader &hdr = ctx.state->header;

                const float rate     = paramFloat("rate", 50.0f);
                const float lifetime = paramFloat("lifetime", 2.0f);
                const Vec3  origin   = paramVec3 ("origin", Vec3(0.0f));
                const Vec3  initialV = paramVec3 ("initial_v", Vec3(0.0f, 1.0f, 0.0f));
                const float jitter   = std::max(0.0f, paramFloat("pos_jitter", 0.0f));
                const int   seed     = paramInt  ("seed", 0);
                const int   emitMode = paramInt  ("emit_mode", 0);

                // Carry-aware emission count. `carry` holds the fractional
                // particles owed from prior substeps; the integer part of
                // (carry + rate*dt) is what we emit this substep, with the
                // remainder rolled forward.
                auto *carryAttr = g.detail().get<float>(
                    "__src_" + std::to_string(uid()) + "_carry");
                auto *nextIdAttr = g.detail().get<int>("__pop_next_id");
                if (!carryAttr || !nextIdAttr) return; // prepare() didn't run

                float &carry = carryAttr->data()[0];
                int &nextId  = nextIdAttr->data()[0];

                carry += rate * static_cast<float>(hdr.dt);
                int emit = static_cast<int>(std::floor(carry));
                if (emit <= 0) return;
                carry -= static_cast<float>(emit);

                // ── Geometry-source mode setup ──
                // Look up the source SOP's cooked geometry through the
                // editor-provided SopGeometryProvider. Falls back to the
                // legacy point-mode path when:
                //   • emit_mode != geometry, or
                //   • no provider is wired (headless / smoke contexts), or
                //   • the source SOP uid hasn't been cooked / was deleted.
                // The fallback path matches the behaviour of an earlier
                // pop_source so existing projects keep working unchanged.
                const Geometry *src = nullptr;
                const Attribute<Vec3>  *srcP  = nullptr;
                const Attribute<Vec3>  *srcN  = nullptr;
                const Attribute<Vec3>  *srcCd = nullptr;
                size_t srcCount = 0;
                if (emitMode == 1 && ctx.sopProvider)
                {
                    const uint64_t srcUid = static_cast<uint64_t>(
                        paramInt("source_sop_uid", 0));
                    if (srcUid != 0)
                        src = ctx.sopProvider->lookupCookedGeometry(srcUid);
                    if (src)
                    {
                        srcP  = src->points().get<Vec3>("P");
                        srcN  = src->points().get<Vec3>("N");
                        srcCd = src->points().get<Vec3>("Cd");
                        if (srcP) srcCount = srcP->data().size();
                    }
                }
                const bool geometryEmit = (emitMode == 1 && src && srcCount > 0);
                const bool useNormal    = paramBool("use_normal", true);
                const float normalSpeed = paramFloat("normal_speed", 1.0f);
                const bool inheritCd    = paramBool("inherit_cd", true);

                // Rotating index into the source's point list — survives
                // substeps so emissions deterministically cycle through
                // the source rather than restarting at 0 every frame.
                int *srcIdxPtr = nullptr;
                if (geometryEmit)
                {
                    auto *idxAttr = g.detail().get<int>(
                        "__src_" + std::to_string(uid()) + "_idx");
                    if (idxAttr) srcIdxPtr = &idxAttr->data()[0];
                }

                // Pre-resize attribute storage once rather than emit-by-emit
                // — addPoint() resizes the whole table for each call, so
                // bulk-emit cost goes from O(n²) to O(n).
                const size_t base = g.pointCount();
                g.points().resize(base + static_cast<size_t>(emit));
                auto *P     = g.points().get<Vec3>("P");
                auto *V     = g.points().get<Vec3>("v");
                auto *AGE   = g.points().get<float>("age");
                auto *LIFE  = g.points().get<float>("life");
                auto *ID    = g.points().get<int>("id");
                auto *FORCE = g.points().get<Vec3>("force");
                if (!P || !V || !AGE || !LIFE || !ID || !FORCE) return;

                // Cd materialised by prepare() when emit_mode=geometry +
                // inherit_cd=true. Stays absent for the legacy point path.
                auto *CD = g.points().get<Vec3>("Cd");

                for (int i = 0; i < emit; ++i)
                {
                    const size_t idx = base + static_cast<size_t>(i);

                    // ── Position + velocity per emit mode ──
                    Vec3 p;
                    Vec3 v;
                    Vec3 cd(1.0f);

                    if (geometryEmit)
                    {
                        // Deterministic round-robin over the source's
                        // point list. Cycling (vs. random index) makes
                        // emission stable across re-cooks and gives the
                        // user a "uniform sweep across the source" feel
                        // even at low rates.
                        const size_t si = static_cast<size_t>(
                            (*srcIdxPtr) % static_cast<int>(srcCount));
                        ++(*srcIdxPtr);
                        p = srcP->data()[si];

                        if (useNormal && srcN && si < srcN->data().size())
                            v = srcN->data()[si] * normalSpeed;
                        else
                            v = initialV;

                        if (inheritCd && srcCd && si < srcCd->data().size())
                            cd = srcCd->data()[si];
                    }
                    else
                    {
                        p = origin;
                        v = initialV;
                    }

                    if (jitter > 0.0f)
                    {
                        // Cheap-ish ball offset: three independent [-1,1]
                        // axes, rejection-sample fallback isn't worth it
                        // for jitter use. Slightly biased toward the cube
                        // corners, fine for emission scatter.
                        const uint32_t hx = hash4i(static_cast<uint32_t>(uid()),
                                                   static_cast<uint32_t>(hdr.frame),
                                                   static_cast<uint32_t>(hdr.substepIdx),
                                                   static_cast<uint32_t>(seed * 7919 + i * 3));
                        const uint32_t hy = hash4i(static_cast<uint32_t>(uid()),
                                                   static_cast<uint32_t>(hdr.frame),
                                                   static_cast<uint32_t>(hdr.substepIdx),
                                                   static_cast<uint32_t>(seed * 7919 + i * 3 + 1));
                        const uint32_t hz = hash4i(static_cast<uint32_t>(uid()),
                                                   static_cast<uint32_t>(hdr.frame),
                                                   static_cast<uint32_t>(hdr.substepIdx),
                                                   static_cast<uint32_t>(seed * 7919 + i * 3 + 2));
                        p.x += (hashToFloat(hx) * 2.0f - 1.0f) * jitter;
                        p.y += (hashToFloat(hy) * 2.0f - 1.0f) * jitter;
                        p.z += (hashToFloat(hz) * 2.0f - 1.0f) * jitter;
                    }
                    P->data()[idx]     = p;
                    V->data()[idx]     = v;
                    AGE->data()[idx]   = 0.0f;
                    LIFE->data()[idx]  = lifetime;
                    ID->data()[idx]    = nextId++;
                    FORCE->data()[idx] = Vec3(0.0f);
                    if (CD) CD->data()[idx] = cd;
                }
            }
        };

        namespace
        {
            template <typename T>
            DopRegistry::Factory makeFactory()
            {
                return [](size_t uid) { return std::make_unique<T>(uid); };
            }
        }

        void registerSourceDops()
        {
            auto &reg = DopRegistry::instance();
            // ParamSpec is aggregate-initialised. Fields after `options`
            // are referenced by name to keep the diff readable when new
            // hints land.
            ParamSpec sourceUidSpec{};
            sourceUidSpec.name = "source_sop_uid";
            sourceUidSpec.type = ParamType::Int;
            sourceUidSpec.defaultRepr = "0";
            sourceUidSpec.picker = "sop_node";
            reg.registerType(
                {"pop_source", "Particle Source", "Source",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                    {"rate",         ParamType::Float, "50.0",       0.0,  10000.0, 1.0},
                    {"lifetime",     ParamType::Float, "2.0",        0.0,  60.0,    0.01},
                    {"emit_mode",    ParamType::Int,   "0",          0.0,   0.0,    0.0,
                                     /*options*/ {"point", "geometry"}},
                    {"origin",       ParamType::Vec3,  "[0, 0, 0]"},
                    {"initial_v",    ParamType::Vec3,  "[0, 1, 0]"},
                    {"pos_jitter",   ParamType::Float, "0.0",        0.0,  10.0,    0.01},
                    sourceUidSpec,
                    {"use_normal",   ParamType::Bool,  "true"},
                    {"normal_speed", ParamType::Float, "1.0",        0.0,  50.0,    0.05},
                    {"inherit_cd",   ParamType::Bool,  "true"},
                    {"seed",         ParamType::Int,   "0"},
                 }},
                makeFactory<PopSourceDop>());
        }
    }
}
