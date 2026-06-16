// Combiner SOP (2-input): sweep a profile curve along a path curve to build a
// tube / ribbon mesh.
//   input 0 = path    (ordered points; the spine)
//   input 1 = profile (ordered points; the cross-section, authored in the XZ
//             ground plane like the curve generators emit)
//
// Each path sample gets a rotation-minimizing frame (B,N,T); the profile's
// (x,z) ride the frame's (B,N) plane perpendicular to the path, its y rides T.
// Consecutive rings are stitched into quads. Profile winding is detected from
// its signed area so the tube faces outward regardless of point order; a
// `flip` toggle inverts it. Closed path/profile wrap; convex end caps optional.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../mograph/curve_frame.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class SweepSop : public SopNode
            {
            public:
                explicit SweepSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeBool("caps", true));
                    declareParam(Parameter::makeBool("flip", false));
                }

                std::string kind() const override { return "sweep"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("path", DataType::Scene3D));
                    io.addInput(PortInfo::createInput("profile", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.size() < 2 || !inputs[0] || !inputs[1]) return {};
                    const Geometry &path = *inputs[0];
                    const Geometry &profile = *inputs[1];
                    const auto &C = path.positions();    // path spine points
                    const auto &Q = profile.positions(); // profile cross-section points
                    const size_t nPath = C.size();
                    const size_t nProf = Q.size();
                    if (nPath < 2 || nProf < 2) return {};

                    const bool pathClosed = mograph::curveClosed(path);
                    const bool profClosed = mograph::curveClosed(profile);
                    const bool caps = paramBool("caps", true);
                    const bool userFlip = paramBool("flip", false);

                    // Frames along the path (B,N,T) from RMF.
                    const auto frames = mograph::computeCurveFrames(C, pathClosed);

                    // Profile 2D coords in the frame's (B,N) plane = (x, z); y → T.
                    std::vector<glm::vec2> q2(nProf);
                    glm::vec2 centroid2(0.0f);
                    for (size_t j = 0; j < nProf; ++j)
                    {
                        q2[j] = glm::vec2(Q[j].x, Q[j].z);
                        centroid2 += q2[j];
                    }
                    centroid2 /= static_cast<float>(nProf);

                    // Signed area decides CCW vs CW so the tube faces outward.
                    float area2 = 0.0f;
                    for (size_t j = 0; j < nProf; ++j)
                    {
                        const glm::vec2 &a = q2[j];
                        const glm::vec2 &b = q2[(j + 1) % nProf];
                        area2 += a.x * b.y - b.x * a.y;
                    }
                    const bool flipWinding = (area2 < 0.0f) != userFlip;

                    // Path arc-length param for the u coordinate.
                    std::vector<float> pcum(nPath, 0.0f);
                    for (size_t i = 1; i < nPath; ++i)
                        pcum[i] = pcum[i - 1] + glm::length(C[i] - C[i - 1]);
                    const float pathLen = std::max(pcum.back(), 1e-8f);

                    const size_t ringCount = nPath;
                    const size_t gridPts = ringCount * nProf;

                    Geometry out;
                    auto &pts = out.points();
                    auto *oP = pts.add<Vec3>("P", Vec3(0.0f));
                    auto *oN = pts.add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f));
                    auto *oUV = pts.add<Vec2>("uv", Vec2(0.0f));
                    // Extra two points for the centroid cap fans.
                    const bool wantCaps = caps && profClosed && !pathClosed;
                    out.resizePoints(gridPts + (wantCaps ? 2 : 0));
                    auto &Pd = oP->data();
                    auto &Nd = oN->data();
                    auto &Ud = oUV->data();

                    auto idx = [&](size_t i, size_t j) { return i * nProf + j; };

                    // Profile per-point outward 2D normal (oriented away from centroid).
                    auto profileNormal2D = [&](size_t j) -> glm::vec2 {
                        const size_t jp = (j + nProf - 1) % nProf;
                        const size_t jn = (j + 1) % nProf;
                        const glm::vec2 tang = q2[jn] - q2[jp];
                        glm::vec2 nrm(tang.y, -tang.x);
                        const float l = glm::length(nrm);
                        nrm = (l > 1e-8f) ? nrm / l : glm::vec2(0.0f);
                        if (glm::dot(nrm, q2[j] - centroid2) < 0.0f) nrm = -nrm;
                        return nrm;
                    };

                    // Build the point grid (position, smooth outward N, uv).
                    for (size_t i = 0; i < ringCount; ++i)
                    {
                        const glm::vec3 Bi = frames[i].B;
                        const glm::vec3 Ni = frames[i].N;
                        const glm::vec3 Ti = frames[i].T;
                        const glm::vec3 Ci = C[i];
                        const float u = pcum[i] / pathLen;
                        for (size_t j = 0; j < nProf; ++j)
                        {
                            const glm::vec3 world = Ci + q2[j].x * Bi + q2[j].y * Ni + Q[j].y * Ti;
                            const glm::vec2 n2 = profileNormal2D(j);
                            const glm::vec3 wN = glm::normalize(n2.x * Bi + n2.y * Ni);
                            const size_t k = idx(i, j);
                            Pd[k] = world;
                            Nd[k] = wN;
                            const float v = profClosed ? static_cast<float>(j) / static_cast<float>(nProf)
                                                       : static_cast<float>(j) / static_cast<float>(nProf - 1);
                            Ud[k] = Vec2(u, v);
                        }
                    }

                    // Stitch rings. (a,b,d)+(a,d,c) is outward for a CCW profile;
                    // flipWinding swaps it for CW profiles / the user toggle.
                    const size_t pathSpans = pathClosed ? nPath : nPath - 1;
                    const size_t profSpans = profClosed ? nProf : nProf - 1;
                    for (size_t i = 0; i < pathSpans; ++i)
                    {
                        const size_t in = (i + 1) % nPath;
                        for (size_t j = 0; j < profSpans; ++j)
                        {
                            const size_t jn = (j + 1) % nProf;
                            const uint32_t a = static_cast<uint32_t>(idx(i, j));
                            const uint32_t b = static_cast<uint32_t>(idx(i, jn));
                            const uint32_t c = static_cast<uint32_t>(idx(in, j));
                            const uint32_t d = static_cast<uint32_t>(idx(in, jn));
                            if (!flipWinding)
                            {
                                out.addTriangle(a, b, d);
                                out.addTriangle(a, d, c);
                            }
                            else
                            {
                                out.addTriangle(a, d, b);
                                out.addTriangle(a, c, d);
                            }
                        }
                    }

                    // Convex end caps (centroid fans) for a closed profile on an open path.
                    if (wantCaps)
                    {
                        const size_t csIdx = gridPts;     // start centroid
                        const size_t ceIdx = gridPts + 1; // end centroid
                        const size_t lastRing = nPath - 1;
                        glm::vec3 cs(0.0f), ce(0.0f);
                        for (size_t j = 0; j < nProf; ++j)
                        {
                            cs += Pd[idx(0, j)];
                            ce += Pd[idx(lastRing, j)];
                        }
                        cs /= static_cast<float>(nProf);
                        ce /= static_cast<float>(nProf);
                        Pd[csIdx] = cs;
                        Pd[ceIdx] = ce;
                        Nd[csIdx] = -frames[0].T;          // start faces back down the path
                        Nd[ceIdx] = frames[lastRing].T;    // end faces forward
                        Ud[csIdx] = Vec2(0.0f, 0.0f);
                        Ud[ceIdx] = Vec2(1.0f, 0.0f);
                        for (size_t j = 0; j < nProf; ++j)
                        {
                            const size_t jn = (j + 1) % nProf;
                            const uint32_t s0 = static_cast<uint32_t>(idx(0, j));
                            const uint32_t s1 = static_cast<uint32_t>(idx(0, jn));
                            const uint32_t e0 = static_cast<uint32_t>(idx(lastRing, j));
                            const uint32_t e1 = static_cast<uint32_t>(idx(lastRing, jn));
                            if (!flipWinding)
                            {
                                out.addTriangle(static_cast<uint32_t>(csIdx), s1, s0);
                                out.addTriangle(static_cast<uint32_t>(ceIdx), e0, e1);
                            }
                            else
                            {
                                out.addTriangle(static_cast<uint32_t>(csIdx), s0, s1);
                                out.addTriangle(static_cast<uint32_t>(ceIdx), e1, e0);
                            }
                        }
                    }

                    return out;
                }
            };
        }

        void registerSweepSop()
        {
            SopRegistry::instance().registerType(
                {"sweep", "Sweep", "Combiners",
                 /*inputs*/ {{"path"}, {"profile"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"caps", ParamType::Bool, "true"},
                     {"flip", ParamType::Bool, "false"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<SweepSop>(uid);
                });
        }
    }
}
