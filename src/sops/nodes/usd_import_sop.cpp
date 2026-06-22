#include "../../geometry/geometry_converter.hpp"
#include "../../scene/scene.hpp"
#include "../../scene/scene_object.hpp"
#include "../../scene/usd_loader.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include <cstdio>
#include <memory>

// OpenUSD import SOP. The exact mirror of gltf_import_sop, so USD assets flow
// through the procedural graph the same way glTF does (the editor's subnet
// importer builds one usd_import → object_output chain per mesh prim). This
// SOP lives in the `tracey_usd` library — NOT core tracey — because it calls
// UsdLoader; the core renderer + C API never link USD. registerUsdImportSop()
// is invoked from the editor (which links tracey_usd) after the builtins.
//
// `mesh_name` holds the prim's full Sdf path — the SceneObject key that
// UsdLoader::loadFromFile registers (and that peekHierarchy emits), so the
// lookup here stays in lockstep with the importer.
namespace tracey
{
    namespace sops
    {
        namespace
        {
            class UsdImportSop : public SopNode
            {
            public:
                explicit UsdImportSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("path", ""));
                    declareParam(Parameter::makeString("mesh_name", ""));
                }

                std::string kind() const override { return "usd_import"; }

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

                    // Shared process-wide parse — a multi-prim import cooks one
                    // usd_import per prim, and apply_emitted re-resolves the
                    // material from the same cache, so the stage is opened once.
                    std::shared_ptr<const Scene> scene = UsdLoader::loadFromFileCached(path);
                    if (!scene)
                    {
                        std::fprintf(stderr, "[usd_import] load failed for %s\n", path.c_str());
                        return Geometry{};
                    }

                    const std::string meshName = paramString("mesh_name", "");
                    if (!meshName.empty())
                    {
                        if (const auto *obj = scene->getObject(meshName))
                        {
                            Geometry g = GeometryConverter::fromSceneObject(*obj);
                            // Stamp source pointers so apply_emitted can recover
                            // the bound USD material (UsdPreviewSurface factors)
                            // and apply it to the live actor — without this every
                            // imported actor renders flat-grey.
                            g.detail().add<std::string>("_usd_source_path", path);
                            g.detail().add<std::string>("_usd_source_mesh", meshName);
                            return g;
                        }
                        std::fprintf(stderr,
                            "[usd_import] prim '%s' not found in %s — emitting empty geometry\n",
                            meshName.c_str(), path.c_str());
                        return Geometry{};
                    }

                    // Bare usd_import → object_output (no prim scoping): merge
                    // every mesh prim, matching gltf_import's back-compat path.
                    Geometry merged;
                    bool first = true;
                    for (const auto &[name, obj] : scene->objects())
                    {
                        Geometry g = GeometryConverter::fromSceneObject(*obj);
                        if (first) { merged = std::move(g); first = false; }
                        else merged.mergeFrom(g);
                    }
                    return merged;
                }
            };
        }

        void registerUsdImportSop()
        {
            SopRegistry::instance().registerType(
                {"usd_import", "USD Import", "Generators",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"path",      ParamType::String, "\"\""},
                     {"mesh_name", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<UsdImportSop>(uid);
                });
        }
    }
}
