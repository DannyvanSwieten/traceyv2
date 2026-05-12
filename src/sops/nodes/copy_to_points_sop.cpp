// Houdini-style "Copy to Points" SOP.
//
// Inputs:
//   0 — stamp:    the geometry to clone.
//   1 — template: a point cloud (only the point table is read; primitives
//                 are ignored).
//
// For each template point, the stamp is transformed by:
//   M = T(P) · R(N → +Z, up = +Y) · S(pscale)
// and concatenated into the output via Geometry::mergeFrom.
//
// Per-instance attribute transfer (v1): a `Cd` Vec3 on the template's point
// table is written as a per-VERTEX `Cd` on every vertex of the resulting
// clone. The data survives through mergeFrom but is currently dropped at
// GeometryConverter::toSceneObject — so it's reachable from later SOPs but
// not yet rendered. A future pass will extend the conversion path + shaders
// to consume per-vertex Cd.
//
// Deferred (matches plan):
//   • `up` per-point + `orient` quat per-point.
//   • Multi-instancing (1 BLAS + N TLAS instances) — current implementation
//     bakes one fat output Geometry, which the existing actor pipeline ships
//     as a single BLAS.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Build the rotation matrix that maps the stamp's local +Z to the
            // direction `n`, using `up` as the up reference. Returns identity
            // when `n` is near-zero or parallel to `up` (no well-defined
            // frame; fall back to no rotation so flat ground points "just
            // work" with up=+Y, N=+Y).
            glm::mat3 orientFromNormal(const Vec3 &n,
                                       const Vec3 &up = Vec3(0.0f, 1.0f, 0.0f))
            {
                const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
                if (len2 < 1e-12f) return glm::mat3(1.0f);
                const float invLen = 1.0f / std::sqrt(len2);
                glm::vec3 forward(n.x * invLen, n.y * invLen, n.z * invLen);

                glm::vec3 upRef(up.x, up.y, up.z);
                glm::vec3 right = glm::cross(upRef, forward);
                const float r2 = glm::dot(right, right);
                if (r2 < 1e-12f) return glm::mat3(1.0f);  // forward parallel to up
                right = right * (1.0f / std::sqrt(r2));
                glm::vec3 newUp = glm::cross(forward, right);

                // Columns are the basis vectors of the rotated frame:
                // X' = right, Y' = newUp, Z' = forward.
                return glm::mat3(right, newUp, forward);
            }

            class CopyToPointsSop : public SopNode
            {
            public:
                explicit CopyToPointsSop(size_t uid) : SopNode(uid)
                {
                    // Default true so existing scenes keep their previous
                    // behaviour (orient each clone's +Z to the template
                    // point's N). Toggle off to ignore N and only apply
                    // translation + pscale — useful for things like grass
                    // blades that should stay axis-aligned regardless of
                    // the underlying terrain's surface normal.
                    declareParam(Parameter::makeBool("orient_to_normal", true));
                }

                std::string kind() const override { return "copy_to_points"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("stamp",    DataType::Scene3D));
                    io.addInput(PortInfo::createInput("template", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out",    DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.size() < 2 || !inputs[0] || !inputs[1]) return {};
                    const Geometry &stamp = *inputs[0];
                    const Geometry &tmpl  = *inputs[1];

                    const auto &tplP  = tmpl.positions();
                    if (tplP.empty()) return {};

                    const auto *tplPs = tmpl.points().get<float>("pscale");
                    const auto *tplN  = tmpl.points().get<Vec3>("N");
                    const auto *tplCd = tmpl.points().get<Vec3>("Cd");
                    const bool useNormal = paramBool("orient_to_normal", true);

                    // If the template carries Cd, ensure the output destination
                    // declares the per-vertex Cd attribute *before* the first
                    // mergeFrom — otherwise mergeFrom drops it (the merge logic
                    // only preserves attributes whose name+type exist on the
                    // destination side).
                    Geometry out;
                    if (tplCd)
                    {
                        out.vertices().add<Vec3>("Cd", Vec3(1.0f));
                    }

                    for (size_t i = 0; i < tplP.size(); ++i)
                    {
                        const Vec3 P = tplP[i];
                        const float s = (tplPs && i < tplPs->data().size())
                                            ? tplPs->data()[i]
                                            : 1.0f;
                        glm::mat3 R(1.0f);
                        if (useNormal && tplN && i < tplN->data().size())
                        {
                            R = orientFromNormal(tplN->data()[i]);
                        }

                        // Structural deep copy of the stamp — we mutate its
                        // positions/normals in place, then mergeFrom into out.
                        Geometry clone = stamp;
                        if (auto *cP = clone.points().get<Vec3>("P"))
                        {
                            for (auto &p : cP->data())
                            {
                                glm::vec3 sp(p.x * s, p.y * s, p.z * s);
                                glm::vec3 tp = R * sp;
                                p = Vec3(tp.x + P.x, tp.y + P.y, tp.z + P.z);
                            }
                        }
                        if (auto *cN = clone.points().get<Vec3>("N"))
                        {
                            for (auto &n : cN->data())
                            {
                                glm::vec3 rn = R * glm::vec3(n.x, n.y, n.z);
                                const float l = std::sqrt(rn.x * rn.x +
                                                          rn.y * rn.y +
                                                          rn.z * rn.z);
                                if (l > 0.0f)
                                    n = Vec3(rn.x / l, rn.y / l, rn.z / l);
                            }
                        }

                        // Per-instance Cd transfer.
                        if (tplCd && i < tplCd->data().size())
                        {
                            const Vec3 cd = tplCd->data()[i];
                            auto *cv = clone.vertices().get<Vec3>("Cd");
                            if (!cv) cv = clone.vertices().add<Vec3>("Cd", Vec3(1.0f));
                            for (auto &v : cv->data()) v = cd;
                        }

                        out.mergeFrom(clone);
                    }
                    return out;
                }
            };
        }

        void registerCopyToPointsSop()
        {
            SopRegistry::instance().registerType(
                {"copy_to_points", "Copy to Points", "Cloners",
                 /*inputs*/ {{"stamp"}, {"template"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"orient_to_normal", ParamType::Bool, "true"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<CopyToPointsSop>(uid);
                });
        }
    }
}
