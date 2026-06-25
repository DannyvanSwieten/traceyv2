#include "../../geometry/geometry_converter.hpp"
#include "../../scene/gltf_loader.hpp"
#include "../../scene/scene.hpp"
#include "../../scene/scene_object.hpp"
#include "../../scene/skeleton.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // FK pose-override param format: whitespace-separated tuples of
            // "jointIndex eulerX eulerY eulerZ" (degrees). Parsed here without a
            // JSON dependency. Returns (jointIndex, euler-degrees) pairs.
            std::vector<std::pair<int, glm::vec3>> parsePoseOverrides(const std::string &s)
            {
                std::vector<std::pair<int, glm::vec3>> out;
                std::istringstream in(s);
                int j;
                float ex, ey, ez;
                while (in >> j >> ex >> ey >> ez)
                    out.emplace_back(j, glm::vec3(ex, ey, ez));
                return out;
            }
            // Linear blend skinning of a SceneObject by per-joint matrices.
            // Returns a deformed copy (positions + normals); other attributes
            // pass through. Vertices with no weight (e.g. non-skinned prims)
            // keep their original position. Normals use the blended matrix's
            // 3x3 — fine for the rigid/near-rigid bones typical of characters
            // (a full inverse-transpose is a later refinement).
            SceneObject skinObject(const SceneObject &src, const std::vector<Mat4> &skin)
            {
                SceneObject out = src;
                const auto &pos = src.positions();
                const auto &ji = src.jointIndices();
                const auto &jw = src.jointWeights();

                std::vector<Vec3> P(pos.size());
                for (size_t i = 0; i < pos.size(); ++i)
                {
                    const Vec4 idx = ji[i];
                    const Vec4 w = jw[i];
                    glm::mat4 m(0.0f);
                    float wsum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float wk = w[k];
                        if (wk == 0.0f)
                            continue;
                        const int j = static_cast<int>(idx[k] + 0.5f);
                        if (j < 0 || j >= static_cast<int>(skin.size()))
                            continue;
                        m += skin[j] * wk;
                        wsum += wk;
                    }
                    if (wsum <= 1e-6f)
                    {
                        P[i] = pos[i];
                        continue;
                    }
                    const Vec4 p = m * Vec4(pos[i], 1.0f);
                    P[i] = Vec3(p);
                }
                out.setPositions(std::move(P));

                if (src.hasNormals())
                {
                    const auto &nrm = src.normals();
                    std::vector<Vec3> N(nrm.size());
                    for (size_t i = 0; i < nrm.size() && i < pos.size(); ++i)
                    {
                        const Vec4 idx = ji[i];
                        const Vec4 w = jw[i];
                        glm::mat3 m(0.0f);
                        float wsum = 0.0f;
                        for (int k = 0; k < 4; ++k)
                        {
                            const float wk = w[k];
                            if (wk == 0.0f)
                                continue;
                            const int j = static_cast<int>(idx[k] + 0.5f);
                            if (j < 0 || j >= static_cast<int>(skin.size()))
                                continue;
                            m += glm::mat3(skin[j]) * wk;
                            wsum += wk;
                        }
                        N[i] = (wsum > 1e-6f) ? glm::normalize(m * nrm[i]) : nrm[i];
                    }
                    out.setNormals(std::move(N));
                }
                return out;
            }

            // Geometry for one SceneObject at time t: skinned if it carries a
            // skeleton, otherwise the plain bind mesh. When `loop` is set the
            // sample time wraps modulo the clip duration so cycles (walk/run/
            // idle) repeat instead of holding the final pose past the clip end.
            Geometry objectGeometryAt(const SceneObject &obj, double time, bool loop,
                                      const std::vector<std::pair<int, glm::vec3>> &jointEulers)
            {
                if (obj.hasSkin())
                {
                    const auto &skel = *obj.skeleton();
                    double t = time;
                    if (loop && !skel.clips.empty())
                    {
                        const double dur = skel.clips[0].duration;
                        if (dur > 1e-6)
                        {
                            t = std::fmod(time, dur);
                            if (t < 0.0)
                                t += dur;
                        }
                    }
                    // FK pose overrides: map each (joint, euler-degrees) onto a
                    // per-node local-rotation delta the skeleton composes on top
                    // of the clip pose.
                    Skeleton::PoseOverrides overrides;
                    if (!jointEulers.empty())
                    {
                        overrides.assign(skel.nodes.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                        for (const auto &[j, e] : jointEulers)
                        {
                            if (j < 0 || static_cast<size_t>(j) >= skel.joints.size())
                                continue;
                            const int node = skel.joints[j];
                            if (node >= 0 && static_cast<size_t>(node) < overrides.size())
                                overrides[node] = glm::quat(glm::radians(e));
                        }
                    }
                    auto skin = skel.skinningMatrices(t, 0,
                                                      overrides.empty() ? nullptr : &overrides);
                    // Express skinning in the mesh node's local space so the
                    // actor transform places the result correctly (the loader
                    // stamped inverse(meshNodeBindWorld) as the bind shift).
                    const Mat4 shift = obj.skinBindShift();
                    for (auto &m : skin)
                        m = shift * m;
                    return GeometryConverter::fromSceneObject(skinObject(obj, skin));
                }
                return GeometryConverter::fromSceneObject(obj);
            }
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
                    // Loop the imported animation clip (wrap sample time modulo
                    // the clip duration). On by default — most character clips
                    // are cycles; turn off for one-shots (jump, hit, death).
                    declareParam(Parameter::makeBool("loop", true));
                    // FK pose overrides — whitespace-separated
                    // "jointIndex eulerX eulerY eulerZ" tuples (degrees), set by
                    // the editor's joint-pose UI. Empty = no overrides.
                    declareParam(Parameter::makeString("pose_overrides", ""));
                }

                std::string kind() const override { return "gltf_import"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                // A skinned + animated import deforms with the playhead even
                // though its params/inputs don't change — mark it time-dependent
                // so SopGraph mixes the cook time into the cache key (otherwise
                // a single frame would be cached and playback would freeze). The
                // editor's per-frame recook trigger consults this too.
                bool isTimeDependent() const override
                {
                    const std::string path = paramString("path", "");
                    if (path.empty()) return SopNode::isTimeDependent();
                    std::shared_ptr<const Scene> scene;
                    try { scene = GltfLoader::loadFromFileCached(path); }
                    catch (...) { return SopNode::isTimeDependent(); }
                    if (!scene) return SopNode::isTimeDependent();
                    auto animated = [](const SceneObject *o) {
                        return o && o->skeleton() && o->skeleton()->animated();
                    };
                    const std::string meshName = paramString("mesh_name", "");
                    if (!meshName.empty())
                    {
                        if (animated(scene->getObject(meshName))) return true;
                    }
                    else
                    {
                        for (const auto &[n, o] : scene->objects())
                            if (animated(o.get())) return true;
                    }
                    return SopNode::isTimeDependent();
                }

                // SopGraph always drives nodes through cookAt, so the skinning
                // pose tracks the playhead. The timeless cook() falls back to
                // t=0 (the clip's first frame / bind pose).
                Geometry cookAt(std::span<const Geometry *const> inputs, double time) const override
                {
                    setEvalTime(time);
                    return build(time);
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    return build(0.0);
                }

            private:
                Geometry build(double time) const
                {
                    const std::string path = paramString("path", "");
                    if (path.empty()) return Geometry{};
                    const bool loop = paramBool("loop", true);
                    const auto jointEulers = parsePoseOverrides(paramString("pose_overrides", ""));

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
                            Geometry g = objectGeometryAt(*obj, time, loop, jointEulers);
                            // Stamp source-of-truth pointers on the geometry's
                            // Detail attributes so the editor's apply_emitted
                            // can look the source glTF up again and apply its
                            // MaterialInstance (base color, factors, textures)
                            // to the live actor's SceneInstance. Without this
                            // the cook strips materials and every imported
                            // actor renders flat-grey.
                            g.detail().add<std::string>("_gltf_source_path", path);
                            g.detail().add<std::string>("_gltf_source_mesh", meshName);
                            // This SOP's node uid, so the editor can find the
                            // node that owns the skeleton (to read pose_overrides
                            // for the bone overlay + drive the joint-pose UI).
                            g.detail().add<std::string>("_gltf_import_node", std::to_string(uid()));
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
                        Geometry g = objectGeometryAt(*obj, time, loop, jointEulers);
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
                     {"path",           ParamType::String, "\"\""},
                     {"mesh_name",      ParamType::String, "\"\""},
                     {"loop",           ParamType::Bool,   "true"},
                     {"pose_overrides", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<GltfImportSop>(uid);
                });
        }
    }
}
