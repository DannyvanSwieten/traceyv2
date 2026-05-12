// NormalSop — recomputes N from face normals. Houdini's "Normal" SOP
// equivalent, plus a flat mode for hard-edged geometry.
//
// Modes:
//   smooth — accumulate face normals into the *point* N attribute,
//            optionally weighted by area / equal / angle. Adjacent
//            faces share a point and therefore a normal, so vertex N
//            on the cooked geometry is left absent and consumers fall
//            back to point N (with linear interpolation in the hit
//            shader).
//   flat   — write the face normal into each *vertex* N slot. Vertices
//            are per-primitive, so a cube's 8 shared points carry 36
//            vertices and every face's three vertices receive their
//            own face normal. The hit shader interpolates vertex N
//            when present (which is constant across a flat face), so
//            cubes render faceted.
//
// `reverse` flips the result at the very end, after weighting and mode
// choice — handy for imported meshes with reversed winding order.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Smoothing weight for a single triangle contribution at a
            // specific corner. Area-weight uses 2× the triangle area
            // (length of the unnormalised cross product); equal-weight
            // gives every face one unit regardless of size; angle-weight
            // uses the interior angle at this corner (the Max '99
            // formulation — usually the visually best smoothing for
            // long thin triangles).
            //
            // `corner` is the index 0/1/2 of the point we're accumulating
            // INTO within the triangle (p0, p1, p2).
            Vec3 weightedFaceContribution(
                const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                int corner, const std::string &weighting)
            {
                const Vec3 e01 = p1 - p0;
                const Vec3 e02 = p2 - p0;
                const Vec3 cross = glm::cross(e01, e02);
                const float crossLen = glm::length(cross);
                if (crossLen < 1e-20f) return Vec3(0.0f);  // degenerate triangle
                const Vec3 unitN = cross / crossLen;

                if (weighting == "equal")  return unitN;
                if (weighting == "area")   return cross;          // length = 2 × area
                // "angle": weight by the interior angle at this corner.
                // The corner's two edges are the two leaving from it,
                // pointing to the other two points.
                Vec3 a, b;
                if      (corner == 0) { a = p1 - p0; b = p2 - p0; }
                else if (corner == 1) { a = p0 - p1; b = p2 - p1; }
                else                  { a = p0 - p2; b = p1 - p2; }
                const float la = glm::length(a), lb = glm::length(b);
                if (la < 1e-20f || lb < 1e-20f) return Vec3(0.0f);
                float cosAngle = glm::dot(a, b) / (la * lb);
                cosAngle = std::max(-1.0f, std::min(1.0f, cosAngle));
                const float angle = std::acos(cosAngle);
                return unitN * angle;
            }

            class NormalSop : public SopNode
            {
            public:
                explicit NormalSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("mode", "smooth"));
                    declareParam(Parameter::makeString("weighting", "area"));
                    declareParam(Parameter::makeBool("reverse", false));
                }

                std::string kind() const override { return "normal"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.empty() || !inputs[0]) return Geometry{};
                    Geometry out = *inputs[0];

                    const auto &positions = out.positions();
                    const auto &prims     = out.primitivesList();
                    const auto &v2p       = out.vertexToPoint();
                    const std::string mode = paramString("mode", "smooth");
                    const std::string weighting = paramString("weighting", "area");
                    const bool reverse = paramBool("reverse", false);
                    const bool flat = (mode == "flat");

                    if (flat)
                    {
                        // Flat mode: per-vertex N = face normal. Remove
                        // any stale point N so downstream consumers
                        // unambiguously pick up the vertex version.
                        out.points().remove("N");
                        auto *Nv = out.vertices().get<Vec3>("N");
                        if (!Nv) Nv = out.vertices().add<Vec3>("N", Vec3(0.0f));
                        auto &N = Nv->data();
                        for (const auto &prim : prims)
                        {
                            if (prim.vertexCount != 3) continue;
                            const uint32_t v0 = prim.firstVertex + 0;
                            const uint32_t v1 = prim.firstVertex + 1;
                            const uint32_t v2 = prim.firstVertex + 2;
                            if (v2 >= v2p.size() || v2 >= N.size()) continue;
                            const uint32_t p0 = v2p[v0];
                            const uint32_t p1 = v2p[v1];
                            const uint32_t p2 = v2p[v2];
                            if (p0 >= positions.size() || p1 >= positions.size() ||
                                p2 >= positions.size()) continue;
                            const Vec3 e1 = positions[p1] - positions[p0];
                            const Vec3 e2 = positions[p2] - positions[p0];
                            Vec3 face = glm::cross(e1, e2);
                            const float len = glm::length(face);
                            if (len > 0.0f) face /= len;
                            if (reverse) face = -face;
                            N[v0] = face;
                            N[v1] = face;
                            N[v2] = face;
                        }
                        return out;
                    }

                    // Smooth mode: per-point N. Remove any stale vertex
                    // N from a previous flat-mode cook so a chain of
                    // smooth→flat→smooth doesn't leave both attributes
                    // floating and have the hit shader pick the wrong one.
                    out.vertices().remove("N");
                    auto *Nattr = out.points().get<Vec3>("N");
                    if (!Nattr) Nattr = out.points().add<Vec3>("N", Vec3(0.0f));
                    auto &N = Nattr->data();
                    std::fill(N.begin(), N.end(), Vec3(0.0f));

                    for (const auto &prim : prims)
                    {
                        if (prim.vertexCount != 3) continue;
                        const uint32_t v0 = prim.firstVertex + 0;
                        const uint32_t v1 = prim.firstVertex + 1;
                        const uint32_t v2 = prim.firstVertex + 2;
                        if (v2 >= v2p.size()) continue;
                        const uint32_t p0 = v2p[v0];
                        const uint32_t p1 = v2p[v1];
                        const uint32_t p2 = v2p[v2];
                        if (p0 >= positions.size() || p1 >= positions.size() ||
                            p2 >= positions.size()) continue;

                        const Vec3 &P0 = positions[p0];
                        const Vec3 &P1 = positions[p1];
                        const Vec3 &P2 = positions[p2];
                        N[p0] += weightedFaceContribution(P0, P1, P2, 0, weighting);
                        N[p1] += weightedFaceContribution(P0, P1, P2, 1, weighting);
                        N[p2] += weightedFaceContribution(P0, P1, P2, 2, weighting);
                    }

                    for (auto &n : N)
                    {
                        const float len = glm::length(n);
                        if (len > 0.0f) n /= len;
                        if (reverse) n = -n;
                    }
                    return out;
                }
            };
        }

        void registerNormalSop()
        {
            SopRegistry::instance().registerType(
                {"normal", "Normal", "Modifiers",
                 /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"mode",      ParamType::String, "\"smooth\""},
                     {"weighting", ParamType::String, "\"area\""},
                     {"reverse",   ParamType::Bool,   "false"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<NormalSop>(uid);
                });
        }
    }
}
