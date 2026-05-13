#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../codegen/transform_compute.hpp"

#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

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

                    // Compose full SRT for positions, and a pure-R matrix
                    // for normals (CPU code ignores scale on normals, so
                    // we match — non-uniform scale gets the same
                    // approximation either path).
                    glm::mat4 Mpos(1.0f);
                    Mpos[0] = glm::vec4(R[0] * s.x, 0.0f);
                    Mpos[1] = glm::vec4(R[1] * s.y, 0.0f);
                    Mpos[2] = glm::vec4(R[2] * s.z, 0.0f);
                    Mpos[3] = glm::vec4(t.x, t.y, t.z, 1.0f);
                    glm::mat4 Mn(R);

                    auto *gpu = codegen::TransformCompute::getGlobal();
                    // Threshold mirrors the VOP compute path: for small
                    // attributes the CPU loop wins (upload + dispatch +
                    // wait > a few-hundred iterations of glm). The
                    // 512-point heuristic comes from attribute_vop_sop.
                    constexpr size_t kGpuThreshold = 512;

                    auto runGpuOrCpu = [&](Attribute<Vec3> *attr,
                                           const glm::mat4 &m,
                                           codegen::TransformCompute::Mode mode,
                                           bool normalizeAfter) {
                        if (!attr) return;
                        const size_t n = attr->size();
                        if (gpu && n >= kGpuThreshold)
                        {
                            float buf[16];
                            std::memcpy(buf, glm::value_ptr(m), sizeof(buf));
                            if (gpu->dispatch(*attr, buf, mode)) return;
                            // Fall through to CPU on dispatch failure.
                        }
                        // CPU fallback. Matches the pre-GPU implementation,
                        // including the "normals ignore scale" choice.
                        auto &d = attr->data();
                        if (mode == codegen::TransformCompute::Mode::Position)
                        {
                            for (auto &p : d)
                            {
                                p = R * (p * s) + t;
                            }
                        }
                        else
                        {
                            for (auto &v : d)
                            {
                                glm::vec3 r = R * glm::vec3(v.x, v.y, v.z);
                                if (normalizeAfter)
                                {
                                    const float l = glm::length(r);
                                    if (l > 0.0f) r /= l;
                                }
                                v = Vec3(r.x, r.y, r.z);
                            }
                        }
                    };

                    runGpuOrCpu(out.points().get<Vec3>("P"), Mpos,
                                codegen::TransformCompute::Mode::Position, false);
                    runGpuOrCpu(out.points().get<Vec3>("N"), Mn,
                                codegen::TransformCompute::Mode::Normal, true);
                    runGpuOrCpu(out.vertices().get<Vec3>("N"), Mn,
                                codegen::TransformCompute::Mode::Normal, true);
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
