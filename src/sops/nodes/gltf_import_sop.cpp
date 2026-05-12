#include "../../geometry/geometry_converter.hpp"
#include "../../scene/gltf_loader.hpp"
#include "../../scene/scene.hpp"
#include "../../scene/scene_object.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include <cstdio>
#include <memory>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // glTF importer SOP. Calls into the existing GltfLoader and
            // returns one of two things:
            //   • `mesh_name` set → exactly that SceneObject's geometry
            //     (used by the recursive-subnet importer in editor_server,
            //     which builds one gltf_import per glTF node).
            //   • `mesh_name` empty → merge of all SceneObjects in the file
            //     (the original single-output behaviour, kept for back-compat
            //     with scenes that drop a bare gltf_import → object_output
            //     pair).
            class GltfImportSop : public SopNode
            {
            public:
                explicit GltfImportSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("path", ""));
                    declareParam(Parameter::makeString("mesh_name", ""));
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

                    // Cached load shared with apply_emitted's material
                    // lookup. Without this, a single cook of a 103-primitive
                    // import re-parses the file 103× and re-decodes every
                    // embedded texture each time — enough memory churn to
                    // stall the worker long enough that the hierarchy
                    // appears to never populate.
                    std::shared_ptr<const Scene> scene;
                    try { scene = GltfLoader::loadFromFileCached(path); }
                    catch (const std::exception& e) {
                        std::fprintf(stderr,
                            "[gltf_import] load failed for %s: %s\n",
                            path.c_str(), e.what());
                        return Geometry{};
                    }
                    if (!scene) return Geometry{};

                    const std::string meshName = paramString("mesh_name", "");
                    if (!meshName.empty())
                    {
                        if (const auto *obj = scene->getObject(meshName))
                        {
                            Geometry g = GeometryConverter::fromSceneObject(*obj);
                            // Stamp source-of-truth pointers on the geometry's
                            // Detail attributes so the editor's apply_emitted
                            // can look the source glTF up again and apply its
                            // MaterialInstance (base color, factors, textures)
                            // to the live actor's SceneInstance. Without this
                            // the cook strips materials and every imported
                            // actor renders flat-grey.
                            g.detail().add<std::string>("_gltf_source_path", path);
                            g.detail().add<std::string>("_gltf_source_mesh", meshName);
                            return g;
                        }
                        // Loud failure: silent empty-geometry results in a
                        // mysterious "no actor" downstream. The most common
                        // cause is a peek/load name mismatch (e.g. non-
                        // triangle primitive in the source mesh that was
                        // skipped at load time). Dumping the path + name
                        // makes this trivial to diagnose without re-running
                        // with a debugger.
                        std::fprintf(stderr,
                            "[gltf_import] mesh '%s' not found in %s — emitting empty geometry\n",
                            meshName.c_str(), path.c_str());
                        return Geometry{};
                    }

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
                 /*params*/ {
                     {"path",      ParamType::String, "\"\""},
                     {"mesh_name", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<GltfImportSop>(uid);
                });
        }
    }
}
