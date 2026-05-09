#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class TransformSop : public SopNode
            {
            public:
                explicit TransformSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeVec3("translate", Vec3(0.0f)));
                    // Euler angles in degrees, applied as Rx then Ry then Rz.
                    declareParam(Parameter::makeVec3("rotate_euler_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("scale", Vec3(1.0f)));
                }

                std::string kind() const override { return "transform"; }

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

                    const Vec3 t = paramVec3("translate", Vec3(0.0f));
                    const Vec3 rDeg = paramVec3("rotate_euler_deg", Vec3(0.0f));
                    const Vec3 s = paramVec3("scale", Vec3(1.0f));

                    const float deg2rad = 3.1415926535f / 180.0f;
                    const Vec3 rRad = rDeg * deg2rad;
                    glm::quat qx = glm::angleAxis(rRad.x, glm::vec3(1, 0, 0));
                    glm::quat qy = glm::angleAxis(rRad.y, glm::vec3(0, 1, 0));
                    glm::quat qz = glm::angleAxis(rRad.z, glm::vec3(0, 0, 1));
                    glm::quat q = qz * qy * qx;
                    glm::mat3 R = glm::mat3_cast(q);

                    auto *P = out.points().get<Vec3>("P");
                    if (P)
                    {
                        for (auto &p : P->data())
                        {
                            // S, then R, then T — standard SRT.
                            p = R * (p * s) + t;
                        }
                    }
                    auto *N = out.points().get<Vec3>("N");
                    if (N)
                    {
                        // For non-uniform scale, this is approximate; for v1
                        // we accept the simplification (Houdini's xform SOP
                        // does the same by default unless "Adjust normals" is
                        // toggled on).
                        for (auto &n : N->data())
                        {
                            n = glm::normalize(R * n);
                        }
                    }
                    auto *Nv = out.vertices().get<Vec3>("N");
                    if (Nv)
                    {
                        for (auto &n : Nv->data())
                        {
                            n = glm::normalize(R * n);
                        }
                    }
                    return out;
                }
            };
        }

        void registerTransformSop()
        {
            SopRegistry::instance().registerType(
                {"transform", "Transform", "Modifiers",
                 /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"translate",        ParamType::Vec3, "[0, 0, 0]"},
                     {"rotate_euler_deg", ParamType::Vec3, "[0, 0, 0]"},
                     {"scale",            ParamType::Vec3, "[1, 1, 1]"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<TransformSop>(uid);
                });
        }
    }
}
