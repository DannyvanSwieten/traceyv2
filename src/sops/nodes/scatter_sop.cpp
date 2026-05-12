// Scatter SOP: distributes `count` points uniformly over the surface of an
// input geometry, area-weighted across triangles. Writes:
//   • P      (Vec3)  — the sampled position
//   • N      (Vec3)  — flat triangle normal at the sampled triangle
//   • pscale (float) — 1.0 (downstream cloners read this as uniform scale)
//
// Deterministic given the `seed` parameter — re-cooks with the same seed
// produce byte-identical results (the PRNG state is local to each cook()).
// The path tracer relies on this: a non-deterministic scatter would reset
// the accumulator implicitly through ever-changing geometry.
//
// Algorithm: build a prefix-sum CDF over triangle areas at cook time,
// uniform-sample [0, total_area) for each point, binary-search the CDF for
// the chosen triangle, then sample a barycentric coordinate via the
// classic √u trick (Osada et al. 2002 / Shirley 1992).

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Tiny xorshift32 — deterministic, cheap, fine for surface
            // sampling. Not for crypto; not for path-tracer importance
            // sampling either.
            struct XorShift32
            {
                uint32_t s;
                explicit XorShift32(uint32_t seed)
                    : s(seed ? seed : 0x12345678u) {}
                uint32_t next()
                {
                    s ^= s << 13;
                    s ^= s >> 17;
                    s ^= s << 5;
                    return s;
                }
                // Uniform float in [0, 1).
                float nextFloat()
                {
                    // Use the top 24 bits — full-precision mantissa for a
                    // float32 in [0,1).
                    return (next() >> 8) * (1.0f / 16777216.0f);
                }
            };

            class ScatterSop : public SopNode
            {
            public:
                explicit ScatterSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeInt("count", 100));
                    declareParam(Parameter::makeInt("seed", 0));
                }

                std::string kind() const override { return "scatter"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in",   DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.empty() || !inputs[0]) return {};
                    const Geometry &in = *inputs[0];

                    const int count = std::max(1, paramInt("count", 100));
                    const int seedParam = paramInt("seed", 0);

                    // Build per-triangle area CDF + per-triangle face normals.
                    const auto &prims = in.primitivesList();
                    const auto &v2p   = in.vertexToPoint();
                    const auto &posIn = in.positions();

                    struct Tri { uint32_t a, b, c; Vec3 n; };
                    std::vector<Tri> tris;
                    std::vector<float> cdf;
                    tris.reserve(prims.size());
                    cdf.reserve(prims.size());

                    float total = 0.0f;
                    for (const auto &prim : prims)
                    {
                        if (prim.vertexCount < 3) continue;
                        // Triangle-fan walk; v1 only emits proper triangles
                        // (vertexCount == 3) but the fan path is forward-safe
                        // for future n-gons.
                        const uint32_t base = prim.firstVertex;
                        for (uint32_t k = 1; k + 1 < prim.vertexCount; ++k)
                        {
                            const uint32_t va = v2p[base];
                            const uint32_t vb = v2p[base + k];
                            const uint32_t vc = v2p[base + k + 1];
                            if (va >= posIn.size() ||
                                vb >= posIn.size() ||
                                vc >= posIn.size()) continue;
                            const Vec3 &pa = posIn[va];
                            const Vec3 &pb = posIn[vb];
                            const Vec3 &pc = posIn[vc];
                            const Vec3 e1(pb.x - pa.x, pb.y - pa.y, pb.z - pa.z);
                            const Vec3 e2(pc.x - pa.x, pc.y - pa.y, pc.z - pa.z);
                            const Vec3 cross(
                                e1.y * e2.z - e1.z * e2.y,
                                e1.z * e2.x - e1.x * e2.z,
                                e1.x * e2.y - e1.y * e2.x);
                            const float crossLen = std::sqrt(
                                cross.x * cross.x +
                                cross.y * cross.y +
                                cross.z * cross.z);
                            const float area = 0.5f * crossLen;
                            if (area <= 0.0f) continue;
                            const Vec3 normal = (crossLen > 0.0f)
                                ? Vec3(cross.x / crossLen,
                                       cross.y / crossLen,
                                       cross.z / crossLen)
                                : Vec3(0.0f, 1.0f, 0.0f);
                            tris.push_back({va, vb, vc, normal});
                            total += area;
                            cdf.push_back(total);
                        }
                    }

                    if (tris.empty() || total <= 0.0f) return {};

                    XorShift32 rng(static_cast<uint32_t>(seedParam) * 0x9E3779B1u +
                                   0xDEADBEEFu);

                    Geometry out;
                    auto &pts = out.points();
                    auto *P  = pts.add<Vec3>("P",  Vec3(0.0f));
                    auto *N  = pts.add<Vec3>("N",  Vec3(0.0f, 1.0f, 0.0f));
                    auto *ps = pts.add<float>("pscale", 1.0f);
                    out.resizePoints(static_cast<size_t>(count));

                    auto &Pd  = P->data();
                    auto &Nd  = N->data();
                    auto &psd = ps->data();

                    for (int i = 0; i < count; ++i)
                    {
                        // Pick a triangle weighted by area.
                        const float u = rng.nextFloat() * total;
                        const auto it = std::upper_bound(cdf.begin(), cdf.end(), u);
                        size_t idx = static_cast<size_t>(it - cdf.begin());
                        if (idx >= tris.size()) idx = tris.size() - 1;
                        const Tri &t = tris[idx];

                        // Uniform barycentric over the triangle.
                        float r1 = rng.nextFloat();
                        float r2 = rng.nextFloat();
                        const float sqrtR1 = std::sqrt(r1);
                        const float b0 = 1.0f - sqrtR1;
                        const float b1 = sqrtR1 * (1.0f - r2);
                        const float b2 = sqrtR1 * r2;

                        const Vec3 &pa = posIn[t.a];
                        const Vec3 &pb = posIn[t.b];
                        const Vec3 &pc = posIn[t.c];
                        Pd[i] = Vec3(b0 * pa.x + b1 * pb.x + b2 * pc.x,
                                     b0 * pa.y + b1 * pb.y + b2 * pc.y,
                                     b0 * pa.z + b1 * pb.z + b2 * pc.z);
                        Nd[i]  = t.n;
                        psd[i] = 1.0f;
                    }
                    return out;
                }
            };
        }

        void registerScatterSop()
        {
            SopRegistry::instance().registerType(
                {"scatter", "Scatter", "Cloners",
                 /*inputs*/ {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"count", ParamType::Int, "100"},
                     {"seed",  ParamType::Int, "0"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<ScatterSop>(uid);
                });
        }
    }
}
