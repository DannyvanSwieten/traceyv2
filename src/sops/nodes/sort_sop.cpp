// SortSop — reorder points by a chosen key. Doesn't change geometry,
// just shuffles the rows of every point attribute (and remaps any
// vertex→point indices on primitives so meshes still render the same
// triangles). Useful upstream of copy_to_points / instance whose
// point order drives the cloning sequence, and as a deterministic
// reshuffle to expose structure (e.g. sort by X to get left-to-right
// numbering before a "fade in" trick).
//
// Modes (`mode`):
//   "axis"     — by `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z` (the `axis` param).
//                Stable sort, so ties preserve original input order.
//   "random"   — shuffle by xorshift32(point_index, seed). Stable across
//                cooks for the same (count, seed) pair.
//   "attribute"— ascending by the .x lane of a scalar/vec3 point attribute.
//
// `reverse` flips the final ordering after the sort completes.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // xorshift32-derived 0..1 hash, same one DeleteSop uses for
            // its fraction mode. Stable per (i, seed).
            float hash01(uint32_t i, uint32_t seed)
            {
                uint32_t x = i + seed * 2654435761u + 374761393u;
                x ^= x >> 13;
                x *= 0x85ebca6bu;
                x ^= x >> 16;
                return float(x & 0x00FFFFFFu) / float(0x01000000u);
            }

            // Apply a permutation to a typed attribute in place: row i in
            // the output comes from row perm[i] in the input. Equivalent
            // to `out.data()[i] = in.data()[perm[i]]` for every i.
            template <typename T>
            void permuteInPlace(Attribute<T> &attr, const std::vector<uint32_t> &perm)
            {
                auto &data = attr.data();
                std::vector<T> tmp(perm.size());
                for (size_t i = 0; i < perm.size(); ++i)
                    tmp[i] = data[perm[i]];
                data = std::move(tmp);
            }

            void permuteTable(AttributeTable &table, const std::vector<uint32_t> &perm)
            {
                const auto names = table.names();
                for (const auto &name : names)
                {
                    if (auto *a = table.get<float>(name)) { permuteInPlace(*a, perm); continue; }
                    if (auto *a = table.get<int>(name))   { permuteInPlace(*a, perm); continue; }
                    if (auto *a = table.get<Vec2>(name))  { permuteInPlace(*a, perm); continue; }
                    if (auto *a = table.get<Vec3>(name))  { permuteInPlace(*a, perm); continue; }
                    if (auto *a = table.get<Vec4>(name))  { permuteInPlace(*a, perm); continue; }
                    // Unknown type — drop.
                    table.remove(name);
                }
            }

            class SortSop : public SopNode
            {
            public:
                explicit SortSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("mode",      "axis"));
                    declareParam(Parameter::makeString("axis",      "+X"));
                    declareParam(Parameter::makeString("attr_name", "P"));
                    declareParam(Parameter::makeInt   ("seed",      0));
                    declareParam(Parameter::makeBool  ("reverse",   false));
                }
                std::string kind() const override { return "sort"; }

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
                    if (n <= 1) return out;

                    const std::string mode = paramString("mode", "axis");

                    // Build perm[newIdx] = oldIdx.
                    std::vector<uint32_t> perm(n);
                    std::iota(perm.begin(), perm.end(), 0u);

                    if (mode == "axis")
                    {
                        const std::string axis = paramString("axis", "+X");
                        const auto &P = out.positions();
                        auto keyOf = [&](uint32_t i) -> float {
                            const Vec3 &p = P[i];
                            if (axis == "+X") return  p.x;
                            if (axis == "-X") return -p.x;
                            if (axis == "+Y") return  p.y;
                            if (axis == "-Y") return -p.y;
                            if (axis == "+Z") return  p.z;
                            if (axis == "-Z") return -p.z;
                            return p.x;
                        };
                        std::stable_sort(perm.begin(), perm.end(),
                                         [&](uint32_t a, uint32_t b) {
                                             return keyOf(a) < keyOf(b);
                                         });
                    }
                    else if (mode == "random")
                    {
                        const uint32_t seed = static_cast<uint32_t>(paramInt("seed", 0));
                        std::vector<float> keys(n);
                        for (size_t i = 0; i < n; ++i)
                            keys[i] = hash01(static_cast<uint32_t>(i), seed);
                        std::stable_sort(perm.begin(), perm.end(),
                                         [&](uint32_t a, uint32_t b) {
                                             return keys[a] < keys[b];
                                         });
                    }
                    else if (mode == "attribute")
                    {
                        const std::string name = paramString("attr_name", "P");
                        std::vector<float> keys(n, 0.0f);
                        if (const auto *a = out.points().get<float>(name))
                        {
                            const auto &d = a->data();
                            for (size_t i = 0; i < n; ++i) keys[i] = d[i];
                        }
                        else if (const auto *a = out.points().get<Vec3>(name))
                        {
                            const auto &d = a->data();
                            for (size_t i = 0; i < n; ++i) keys[i] = d[i].x;
                        }
                        // Missing attribute → all keys 0; sort is a no-op
                        // (stable sort preserves input order).
                        std::stable_sort(perm.begin(), perm.end(),
                                         [&](uint32_t a, uint32_t b) {
                                             return keys[a] < keys[b];
                                         });
                    }

                    if (paramBool("reverse", false))
                        std::reverse(perm.begin(), perm.end());

                    // No-op shortcut: sort produced the identity permutation.
                    bool identity = true;
                    for (size_t i = 0; i < n; ++i)
                        if (perm[i] != static_cast<uint32_t>(i)) { identity = false; break; }
                    if (identity) return out;

                    // Apply the permutation to every point attribute and
                    // remap any vertex→point indices so meshes still
                    // reference the same physical points after the shuffle.
                    permuteTable(out.points(), perm);

                    // oldToNew[oldIdx] = newIdx — needed to rewrite v2p.
                    std::vector<uint32_t> oldToNew(n);
                    for (size_t newIdx = 0; newIdx < n; ++newIdx)
                        oldToNew[perm[newIdx]] = static_cast<uint32_t>(newIdx);
                    auto &v2p = out.vertexToPoint();
                    for (auto &p : v2p)
                        if (p < n) p = oldToNew[p];

                    return out;
                }
            };
        }  // anon

        void registerSortSop()
        {
            SopRegistry::instance().registerType(
                {"sort", "Sort", "Modifiers",
                 /*inputs*/  {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"mode",      ParamType::String, "\"axis\""},
                     {"axis",      ParamType::String, "\"+X\""},
                     {"attr_name", ParamType::String, "\"P\""},
                     {"seed",      ParamType::Int,    "0"},
                     {"reverse",   ParamType::Bool,   "false"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<SortSop>(uid);
                });
        }
    }
}
