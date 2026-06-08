// DeleteSop — remove points (and any primitives that reference them)
// from the input geometry. The point-based case is the workhorse for
// particle pipelines (drop dead particles, cull out-of-bounds points);
// the mesh-aware case lets you slice geometry by region.
//
// Selection modes (`mode` param):
//   "bbox"      — keep only points inside [min, max], or outside when
//                 `invert` is true. Bounds default to a unit cube.
//   "attribute" — compare a scalar attribute (the .x lane of vec3
//                 attributes — same convention the VOP eval uses) against
//                 a threshold via the chosen `op`. Keeps the points that
//                 PASS the predicate; flipping `invert` deletes them.
//   "fraction"  — random subsample. Keeps `keep_fraction` of the input
//                 points (seeded by `seed` for stable cooks).
//
// Mesh-aware behaviour: a primitive is dropped if any of its referenced
// points were deleted. The remaining primitives have their vertex table
// compacted and their vertex→point indices remapped.
//
// v1 supports compaction for float / int / Vec2 / Vec3 / Vec4 attributes
// — that's every numeric type the rest of the engine actually uses.
// Anything else (matrices, strings) is silently dropped from the output;
// callers that care can re-author those attributes downstream.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // xorshift32-based 0..1 hash for the fraction mode. Stable
            // across cooks for the same (point_index, seed) pair, so
            // toggling the SOP doesn't shuffle the surviving subset.
            float hash01(uint32_t i, uint32_t seed)
            {
                uint32_t x = i + seed * 2654435761u + 374761393u;
                x ^= x >> 13;
                x *= 0x85ebca6bu;
                x ^= x >> 16;
                return float(x & 0x00FFFFFFu) / float(0x01000000u);
            }

            // Apply a per-element comparison op. The names mirror Houdini's
            // delete-by-expression vocabulary.
            bool compareScalar(float v, std::string_view op, float threshold)
            {
                if (op == ">")  return v >  threshold;
                if (op == ">=") return v >= threshold;
                if (op == "<")  return v <  threshold;
                if (op == "<=") return v <= threshold;
                if (op == "==") return v == threshold;
                if (op == "!=") return v != threshold;
                return false;  // unrecognised → keep nothing
            }

            // Compact `attr` in place to the rows listed by `keepOldIdx`,
            // preserving order. After the call attr.size() == keepOldIdx.size().
            template <typename T>
            void compactInPlace(Attribute<T> &attr,
                                const std::vector<uint32_t> &keepOldIdx)
            {
                auto &data = attr.data();
                for (size_t newIdx = 0; newIdx < keepOldIdx.size(); ++newIdx)
                {
                    const uint32_t src = keepOldIdx[newIdx];
                    if (src != newIdx) data[newIdx] = data[src];
                }
                attr.resize(keepOldIdx.size());
            }

            // Walk every attribute on `table` and compact those we know how
            // to (numeric types). Attributes we can't compact get dropped —
            // we'd rather lose a stray string attribute than silently leave
            // it stale-sized after a delete.
            void compactTable(AttributeTable &table,
                              const std::vector<uint32_t> &keepOldIdx)
            {
                const auto names = table.names();
                for (const auto &name : names)
                {
                    if (auto *a = table.get<float>(name)) { compactInPlace(*a, keepOldIdx); continue; }
                    if (auto *a = table.get<int>(name))   { compactInPlace(*a, keepOldIdx); continue; }
                    if (auto *a = table.get<Vec2>(name))  { compactInPlace(*a, keepOldIdx); continue; }
                    if (auto *a = table.get<Vec3>(name))  { compactInPlace(*a, keepOldIdx); continue; }
                    if (auto *a = table.get<Vec4>(name))  { compactInPlace(*a, keepOldIdx); continue; }
                    // Unknown type — drop rather than leave stale-sized.
                    table.remove(name);
                }
            }

            class DeleteSop : public SopNode
            {
            public:
                explicit DeleteSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("mode",      "bbox"));
                    declareParam(Parameter::makeBool  ("invert",    false));
                    // bbox params
                    declareParam(Parameter::makeVec3  ("bbox_min",  Vec3(-1.0f)));
                    declareParam(Parameter::makeVec3  ("bbox_max",  Vec3( 1.0f)));
                    // attribute-predicate params
                    declareParam(Parameter::makeString("attr_name", "Cd"));
                    declareParam(Parameter::makeString("op",        ">"));
                    declareParam(Parameter::makeFloat ("threshold", 0.5f));
                    // fraction params
                    declareParam(Parameter::makeFloat ("keep_fraction", 0.5f));
                    declareParam(Parameter::makeInt   ("seed",          0));
                }

                std::string kind() const override { return "delete"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput (PortInfo::createInput ("in",  DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.empty() || !inputs[0]) return Geometry{};
                    Geometry out = *inputs[0];
                    const size_t n = out.pointCount();
                    if (n == 0) return out;

                    const std::string mode   = paramString("mode", "bbox");
                    const bool        invert = paramBool("invert", false);

                    // Build a per-point "keep" mask. The predicate flavour
                    // depends on `mode`; `invert` just flips the result.
                    std::vector<uint8_t> keep(n, 1);
                    if (mode == "bbox")
                    {
                        const Vec3 lo = paramVec3("bbox_min", Vec3(-1.0f));
                        const Vec3 hi = paramVec3("bbox_max", Vec3( 1.0f));
                        const auto &P = out.positions();
                        for (size_t i = 0; i < n; ++i)
                        {
                            const Vec3 &p = P[i];
                            const bool inside =
                                p.x >= lo.x && p.x <= hi.x &&
                                p.y >= lo.y && p.y <= hi.y &&
                                p.z >= lo.z && p.z <= hi.z;
                            keep[i] = inside ? 1 : 0;
                        }
                    }
                    else if (mode == "attribute")
                    {
                        const std::string name = paramString("attr_name", "Cd");
                        const std::string op   = paramString("op", ">");
                        const float t          = paramFloat("threshold", 0.5f);
                        // Probe float first, then vec3 (taking .x — same
                        // convention the CPU VOP evaluator uses for
                        // vec3→scalar coercion). Missing attribute →
                        // every point fails the predicate (keep=0) so the
                        // user gets a hard signal instead of a silent
                        // no-op.
                        if (const auto *a = out.points().get<float>(name))
                        {
                            const auto &d = a->data();
                            for (size_t i = 0; i < n; ++i)
                                keep[i] = compareScalar(d[i], op, t) ? 1 : 0;
                        }
                        else if (const auto *a = out.points().get<Vec3>(name))
                        {
                            const auto &d = a->data();
                            for (size_t i = 0; i < n; ++i)
                                keep[i] = compareScalar(d[i].x, op, t) ? 1 : 0;
                        }
                        else
                        {
                            // Attribute missing → nothing passes.
                            std::fill(keep.begin(), keep.end(), 0);
                        }
                    }
                    else if (mode == "fraction")
                    {
                        const float    f    = std::clamp(paramFloat("keep_fraction", 0.5f), 0.0f, 1.0f);
                        const uint32_t seed = static_cast<uint32_t>(paramInt("seed", 0));
                        for (size_t i = 0; i < n; ++i)
                            keep[i] = (hash01(static_cast<uint32_t>(i), seed) < f) ? 1 : 0;
                    }

                    if (invert)
                        for (auto &k : keep) k = k ? 0 : 1;

                    // Build the surviving-points list. If everyone passes,
                    // short-circuit so the cook is a true no-op.
                    std::vector<uint32_t> keepOldIdx;
                    keepOldIdx.reserve(n);
                    for (size_t i = 0; i < n; ++i)
                        if (keep[i]) keepOldIdx.push_back(static_cast<uint32_t>(i));
                    if (keepOldIdx.size() == n) return out;

                    // old point id → new point id, or UINT32_MAX if deleted.
                    // We use this both to remap vertex→point indices on
                    // surviving primitives and to test whether a primitive's
                    // points all survived.
                    std::vector<uint32_t> oldToNew(n, UINT32_MAX);
                    for (uint32_t newIdx = 0; newIdx < keepOldIdx.size(); ++newIdx)
                        oldToNew[keepOldIdx[newIdx]] = newIdx;

                    compactTable(out.points(), keepOldIdx);

                    // ── Mesh side ──
                    // A primitive survives only if every one of its vertices
                    // points to a kept point. We rebuild the vertex+primitive
                    // tables in a single pass, accumulating which vertex
                    // indices to keep so the per-attribute compact below
                    // matches.
                    auto &primList = out.primitivesList();
                    auto &v2p      = out.vertexToPoint();
                    if (!primList.empty())
                    {
                        std::vector<uint32_t>  keepVerts;
                        std::vector<GeoPrimitive> newPrims;
                        std::vector<uint32_t>  newV2p;
                        keepVerts.reserve(v2p.size());
                        newPrims.reserve(primList.size());
                        newV2p.reserve(v2p.size());
                        for (const auto &prim : primList)
                        {
                            // First pass: is every vertex's point kept?
                            bool allKept = true;
                            for (uint32_t v = 0; v < prim.vertexCount; ++v)
                            {
                                const uint32_t pOld = v2p[prim.firstVertex + v];
                                if (pOld >= n || oldToNew[pOld] == UINT32_MAX)
                                {
                                    allKept = false;
                                    break;
                                }
                            }
                            if (!allKept) continue;

                            GeoPrimitive np{};
                            np.firstVertex = static_cast<uint32_t>(newV2p.size());
                            np.vertexCount = prim.vertexCount;
                            for (uint32_t v = 0; v < prim.vertexCount; ++v)
                            {
                                const uint32_t vOld = prim.firstVertex + v;
                                keepVerts.push_back(vOld);
                                newV2p.push_back(oldToNew[v2p[vOld]]);
                            }
                            newPrims.push_back(np);
                        }
                        primList = std::move(newPrims);
                        v2p      = std::move(newV2p);
                        compactTable(out.vertices(), keepVerts);
                    }

                    return out;
                }
            };
        }  // anon

        void registerDeleteSop()
        {
            SopRegistry::instance().registerType(
                {"delete", "Delete", "Modifiers",
                 /*inputs*/  {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"mode",          ParamType::String, "\"bbox\""},
                     {"invert",        ParamType::Bool,   "false"},
                     {"bbox_min",      ParamType::Vec3,   "[-1, -1, -1]"},
                     {"bbox_max",      ParamType::Vec3,   "[1, 1, 1]"},
                     {"attr_name",     ParamType::String, "\"Cd\""},
                     {"op",            ParamType::String, "\">\""},
                     {"threshold",     ParamType::Float,  "0.5"},
                     {"keep_fraction", ParamType::Float,  "0.5"},
                     {"seed",          ParamType::Int,    "0"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<DeleteSop>(uid);
                });
        }
    }
}
