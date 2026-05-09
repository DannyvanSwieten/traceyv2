// Generator SOPs that wrap the existing SceneObject primitive factories.
// Each one runs the corresponding tracey::SceneObject::create*() and feeds
// the result through GeometryConverter::fromSceneObject so it lands in the
// SOP graph as a regular Geometry.

#include "../../geometry/geometry_converter.hpp"
#include "../../scene/scene_object.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class PrimitiveCubeSop : public SopNode
            {
            public:
                explicit PrimitiveCubeSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("size", 1.0f));
                }
                std::string kind() const override { return "primitive_cube"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const float size = paramFloat("size", 1.0f);
                    return GeometryConverter::fromSceneObject(SceneObject::createCube(size));
                }
            };

            class PrimitiveSphereSop : public SopNode
            {
            public:
                explicit PrimitiveSphereSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("radius", 1.0f));
                    declareParam(Parameter::makeInt("segments", 16));
                    declareParam(Parameter::makeInt("rings", 16));
                }
                std::string kind() const override { return "primitive_sphere"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return GeometryConverter::fromSceneObject(SceneObject::createSphere(
                        paramFloat("radius", 1.0f),
                        static_cast<uint32_t>(paramInt("segments", 16)),
                        static_cast<uint32_t>(paramInt("rings", 16))));
                }
            };

            class PrimitivePlaneSop : public SopNode
            {
            public:
                explicit PrimitivePlaneSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("width", 1.0f));
                    declareParam(Parameter::makeFloat("depth", 1.0f));
                }
                std::string kind() const override { return "primitive_plane"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return GeometryConverter::fromSceneObject(SceneObject::createPlane(
                        paramFloat("width", 1.0f), paramFloat("depth", 1.0f)));
                }
            };

            class PrimitiveTorusSop : public SopNode
            {
            public:
                explicit PrimitiveTorusSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("major_radius", 1.0f));
                    declareParam(Parameter::makeFloat("minor_radius", 0.3f));
                    declareParam(Parameter::makeInt("major_segments", 32));
                    declareParam(Parameter::makeInt("minor_segments", 16));
                }
                std::string kind() const override { return "primitive_torus"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return GeometryConverter::fromSceneObject(SceneObject::createTorus(
                        paramFloat("major_radius", 1.0f),
                        paramFloat("minor_radius", 0.3f),
                        static_cast<uint32_t>(paramInt("major_segments", 32)),
                        static_cast<uint32_t>(paramInt("minor_segments", 16))));
                }
            };

            class PrimitiveCylinderSop : public SopNode
            {
            public:
                explicit PrimitiveCylinderSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("radius", 0.5f));
                    declareParam(Parameter::makeFloat("height", 1.0f));
                    declareParam(Parameter::makeInt("segments", 32));
                }
                std::string kind() const override { return "primitive_cylinder"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return GeometryConverter::fromSceneObject(SceneObject::createCylinder(
                        paramFloat("radius", 0.5f),
                        paramFloat("height", 1.0f),
                        static_cast<uint32_t>(paramInt("segments", 32))));
                }
            };

            class PrimitiveConeSop : public SopNode
            {
            public:
                explicit PrimitiveConeSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("radius", 0.5f));
                    declareParam(Parameter::makeFloat("height", 1.0f));
                    declareParam(Parameter::makeInt("segments", 32));
                }
                std::string kind() const override { return "primitive_cone"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }
                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return GeometryConverter::fromSceneObject(SceneObject::createCone(
                        paramFloat("radius", 0.5f),
                        paramFloat("height", 1.0f),
                        static_cast<uint32_t>(paramInt("segments", 32))));
                }
            };
        } // anon

        template <typename T>
        SopRegistry::Factory makeFactory()
        {
            return [](size_t uid) -> std::unique_ptr<SopNode> {
                return std::make_unique<T>(uid);
            };
        }

        void registerPrimitiveSops()
        {
            auto &reg = SopRegistry::instance();
            reg.registerType(
                {"primitive_cube", "Cube", "Generators",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"size", ParamType::Float, "1.0"}}},
                makeFactory<PrimitiveCubeSop>());
            reg.registerType(
                {"primitive_sphere", "Sphere", "Generators",
                 {}, {{"out"}},
                 {{"radius",   ParamType::Float, "1.0"},
                  {"segments", ParamType::Int,   "16"},
                  {"rings",    ParamType::Int,   "16"}}},
                makeFactory<PrimitiveSphereSop>());
            reg.registerType(
                {"primitive_plane", "Plane", "Generators",
                 {}, {{"out"}},
                 {{"width", ParamType::Float, "1.0"},
                  {"depth", ParamType::Float, "1.0"}}},
                makeFactory<PrimitivePlaneSop>());
            reg.registerType(
                {"primitive_torus", "Torus", "Generators",
                 {}, {{"out"}},
                 {{"major_radius",   ParamType::Float, "1.0"},
                  {"minor_radius",   ParamType::Float, "0.3"},
                  {"major_segments", ParamType::Int,   "32"},
                  {"minor_segments", ParamType::Int,   "16"}}},
                makeFactory<PrimitiveTorusSop>());
            reg.registerType(
                {"primitive_cylinder", "Cylinder", "Generators",
                 {}, {{"out"}},
                 {{"radius",   ParamType::Float, "0.5"},
                  {"height",   ParamType::Float, "1.0"},
                  {"segments", ParamType::Int,   "32"}}},
                makeFactory<PrimitiveCylinderSop>());
            reg.registerType(
                {"primitive_cone", "Cone", "Generators",
                 {}, {{"out"}},
                 {{"radius",   ParamType::Float, "0.5"},
                  {"height",   ParamType::Float, "1.0"},
                  {"segments", ParamType::Int,   "32"}}},
                makeFactory<PrimitiveConeSop>());
        }
    }
}
