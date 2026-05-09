#include "../../geometry/geometry_converter.hpp"
#include "../../scene/gltf_loader.hpp"
#include "../../scene/scene.hpp"
#include "../../scene/scene_object.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // glTF importer SOP. Calls into the existing GltfLoader, then
            // either picks the first SceneObject or merges them all (the v1
            // behaviour is "merge all" so a multi-mesh glb produces one
            // Geometry — single Object Output downstream gets all of it.
            // A future ForEachObject SOP could split the Scene's objects
            // into independent Geometries via ports).
            class GltfImportSop : public SopNode
            {
            public:
                explicit GltfImportSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("path", ""));
                }

                std::string kind() const override { return "gltf_import"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const std::string path = paramString("path", "");
                    if (path.empty()) return Geometry{};

                    auto scene = GltfLoader::loadFromFile(path);
                    if (!scene) return Geometry{};

                    Geometry merged;
                    bool first = true;
                    for (const auto &[name, obj] : scene->objects())
                    {
                        Geometry g = GeometryConverter::fromSceneObject(*obj);
                        if (first)
                        {
                            merged = std::move(g);
                            first = false;
                        }
                        else
                        {
                            // mergeFrom requires the destination to declare
                            // the attributes that should survive. P (Point)
                            // is always present; N and uv get merged if the
                            // first object had them too. Mismatched
                            // attributes are dropped — usually fine when all
                            // objects in a single glb share the same vertex
                            // shape.
                            merged.mergeFrom(g);
                        }
                    }
                    return merged;
                }
            };
        }

        void registerGltfImportSop()
        {
            SopRegistry::instance().registerType(
                {"gltf_import", "glTF Import", "Generators",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"path", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<GltfImportSop>(uid);
                });
        }
    }
}
