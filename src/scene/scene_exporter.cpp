#include "scene_exporter.hpp"

#include "scene.hpp"
#include "scene_object.hpp"
#include "scene_instance.hpp"
#include "material_instance.hpp"
#include "actor.hpp"
#include "transform.hpp"
#include "../core/types.hpp"

// Declarations only — TINYGLTF_IMPLEMENTATION (and the stb writers) are
// compiled exactly once, in gltf_loader.cpp. This TU and that one live in the
// same `tracey` static library, so the linker resolves WriteGltfSceneToFile to
// the single existing definition. Do NOT define any *_IMPLEMENTATION macro here.
#include <tiny_gltf.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracey
{
    namespace
    {
        // ---- Resolved material values (engine defaults filled in) ----------
        // Mirrors GPUMaterial's defaults so a re-import reproduces the same
        // look even when a MaterialInstance leaves a property unset.
        struct ResolvedMaterial
        {
            Vec3 albedo{1.0f, 1.0f, 1.0f};
            float opacity = 1.0f;
            float metallic = 0.0f;   // GPUMaterial default
            float roughness = 0.5f;  // GPUMaterial default
            Vec3 emission{0.0f, 0.0f, 0.0f};
            float transmission = 0.0f;
            float ior = 1.5f;
            float emissionStrength = 1.0f;
        };

        ResolvedMaterial resolveMaterial(const MaterialInstance &m)
        {
            ResolvedMaterial r;
            if (auto a = m.albedo()) r.albedo = *a;
            if (auto o = m.getFloat("opacity")) r.opacity = *o;
            if (auto mt = m.metallic()) r.metallic = *mt;
            if (auto rg = m.roughness()) r.roughness = *rg;
            if (auto e = m.emission()) r.emission = *e;
            if (auto t = m.getFloat("transmission")) r.transmission = *t;
            if (auto i = m.getFloat("ior")) r.ior = *i;
            if (auto es = m.getFloat("emissionStrength")) r.emissionStrength = *es;
            return r;
        }

        // Stable content key so identical materials collapse to one entry.
        std::string materialKey(const ResolvedMaterial &m)
        {
            std::ostringstream os;
            os.precision(6);
            os << m.albedo.x << ',' << m.albedo.y << ',' << m.albedo.z << ',' << m.opacity << '|'
               << m.metallic << ',' << m.roughness << '|'
               << m.emission.x << ',' << m.emission.y << ',' << m.emission.z << '|'
               << m.transmission << ',' << m.ior << ',' << m.emissionStrength;
            return os.str();
        }

        // ---- One draw = one instance: a SceneObject + its baked world matrix
        // + the per-instance material -------------------------------------
        struct Draw
        {
            Mat4 world;
            const SceneObject *obj;
            const MaterialInstance *mat;
        };

        std::vector<Draw> collectDraws(const Scene &scene)
        {
            std::vector<Draw> draws;
            for (const auto &node : scene.flatten())
            {
                const Actor *actor = node.actor;
                if (!actor || !actor->visible()) continue;
                for (const auto &inst : actor->instances())
                {
                    const SceneObject *obj = scene.getObject(inst.objectRef());
                    if (!obj || obj->positions().empty()) continue;
                    Mat4 world = node.worldTransform;
                    if (inst.hasLocalTransform())
                        world = world * inst.localTransform()->toMatrix();
                    draws.push_back({world, obj, &inst.material()});
                }
            }
            return draws;
        }

        // Triangle index list, synthesising a sequential soup when the object
        // carries no explicit indices (mirrors SceneCompiler's handling).
        std::vector<uint32_t> triangleIndices(const SceneObject &obj)
        {
            const auto &idx = obj.indices();
            if (!idx.empty()) return idx;
            std::vector<uint32_t> seq(obj.positions().size());
            for (uint32_t i = 0; i < seq.size(); ++i) seq[i] = i;
            return seq;
        }

        // =====================================================================
        // glTF / GLB
        // =====================================================================

        // Append bytes to buffer 0 at a 4-byte-aligned offset and return a new
        // bufferView index. All our data is float/uint32 (4-byte), so keeping
        // every view 4-byte aligned satisfies the glTF accessor alignment rule.
        int addBufferView(tinygltf::Model &model, const void *data, size_t byteLength, int target)
        {
            auto &buf = model.buffers[0];
            while (buf.data.size() % 4 != 0) buf.data.push_back(0);
            const size_t offset = buf.data.size();
            const auto *p = static_cast<const unsigned char *>(data);
            buf.data.insert(buf.data.end(), p, p + byteLength);

            tinygltf::BufferView bv;
            bv.buffer = 0;
            bv.byteOffset = offset;
            bv.byteLength = byteLength;
            bv.target = target;
            model.bufferViews.push_back(bv);
            return static_cast<int>(model.bufferViews.size()) - 1;
        }

        int addAccessor(tinygltf::Model &model, int bufferView, int componentType,
                        int type, size_t count)
        {
            tinygltf::Accessor acc;
            acc.bufferView = bufferView;
            acc.byteOffset = 0;
            acc.componentType = componentType;
            acc.type = type;
            acc.count = count;
            model.accessors.push_back(acc);
            return static_cast<int>(model.accessors.size()) - 1;
        }

        // Vertex accessors for one SceneObject (deduped — built once per name).
        struct GeoAccessors
        {
            int position = -1;
            int normal = -1;
            int uv = -1;
            int color = -1;
            int indices = -1;
        };

        GeoAccessors buildGeoAccessors(tinygltf::Model &model, const SceneObject &obj)
        {
            GeoAccessors g;
            const auto &P = obj.positions();
            const size_t n = P.size();

            // POSITION (with required min/max).
            {
                std::vector<float> data(n * 3);
                Vec3 lo = P[0], hi = P[0];
                for (size_t i = 0; i < n; ++i)
                {
                    data[i * 3 + 0] = P[i].x;
                    data[i * 3 + 1] = P[i].y;
                    data[i * 3 + 2] = P[i].z;
                    lo = glm::min(lo, P[i]);
                    hi = glm::max(hi, P[i]);
                }
                int bv = addBufferView(model, data.data(), data.size() * sizeof(float),
                                       TINYGLTF_TARGET_ARRAY_BUFFER);
                g.position = addAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                         TINYGLTF_TYPE_VEC3, n);
                model.accessors[g.position].minValues = {lo.x, lo.y, lo.z};
                model.accessors[g.position].maxValues = {hi.x, hi.y, hi.z};
            }

            if (obj.hasNormals() && obj.normals().size() == n)
            {
                const auto &N = obj.normals();
                std::vector<float> data(n * 3);
                for (size_t i = 0; i < n; ++i)
                {
                    data[i * 3 + 0] = N[i].x;
                    data[i * 3 + 1] = N[i].y;
                    data[i * 3 + 2] = N[i].z;
                }
                int bv = addBufferView(model, data.data(), data.size() * sizeof(float),
                                       TINYGLTF_TARGET_ARRAY_BUFFER);
                g.normal = addAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                       TINYGLTF_TYPE_VEC3, n);
            }

            if (obj.hasUvs() && obj.uvs().size() == n)
            {
                const auto &UV = obj.uvs();
                std::vector<float> data(n * 2);
                for (size_t i = 0; i < n; ++i)
                {
                    data[i * 2 + 0] = UV[i].x;
                    data[i * 2 + 1] = UV[i].y;
                }
                int bv = addBufferView(model, data.data(), data.size() * sizeof(float),
                                       TINYGLTF_TARGET_ARRAY_BUFFER);
                g.uv = addAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                   TINYGLTF_TYPE_VEC2, n);
            }

            if (obj.hasColors() && obj.colors().size() == n)
            {
                const auto &C = obj.colors();
                std::vector<float> data(n * 3);
                for (size_t i = 0; i < n; ++i)
                {
                    data[i * 3 + 0] = C[i].x;
                    data[i * 3 + 1] = C[i].y;
                    data[i * 3 + 2] = C[i].z;
                }
                int bv = addBufferView(model, data.data(), data.size() * sizeof(float),
                                       TINYGLTF_TARGET_ARRAY_BUFFER);
                g.color = addAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                      TINYGLTF_TYPE_VEC3, n);
            }

            {
                std::vector<uint32_t> idx = triangleIndices(obj);
                int bv = addBufferView(model, idx.data(), idx.size() * sizeof(uint32_t),
                                       TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                g.indices = addAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                        TINYGLTF_TYPE_SCALAR, idx.size());
            }

            return g;
        }

        // Build a glTF material from resolved factors. Reverse of GltfLoader's
        // material import: baseColorFactor(+alpha), metallic/roughness,
        // emissiveFactor, and the KHR transmission/ior/emissive_strength
        // extensions; alphaMode BLEND when opacity < 1.
        int buildMaterial(tinygltf::Model &model, std::set<std::string> &extsUsed,
                          const ResolvedMaterial &m, int index)
        {
            tinygltf::Material gm;
            gm.name = "material_" + std::to_string(index);

            gm.pbrMetallicRoughness.baseColorFactor = {m.albedo.x, m.albedo.y, m.albedo.z, m.opacity};
            gm.pbrMetallicRoughness.metallicFactor = m.metallic;
            gm.pbrMetallicRoughness.roughnessFactor = m.roughness;

            // emissiveFactor is spec-clamped to [0,1]; HDR scale rides on the
            // KHR_materials_emissive_strength extension below.
            gm.emissiveFactor = {std::clamp(m.emission.x, 0.0f, 1.0f),
                                 std::clamp(m.emission.y, 0.0f, 1.0f),
                                 std::clamp(m.emission.z, 0.0f, 1.0f)};

            if (m.opacity < 1.0f)
                gm.alphaMode = "BLEND";

            if (m.transmission > 0.0f)
            {
                tinygltf::Value::Object o;
                o["transmissionFactor"] = tinygltf::Value(static_cast<double>(m.transmission));
                gm.extensions["KHR_materials_transmission"] = tinygltf::Value(o);
                extsUsed.insert("KHR_materials_transmission");
            }
            if (m.ior != 1.5f)
            {
                tinygltf::Value::Object o;
                o["ior"] = tinygltf::Value(static_cast<double>(m.ior));
                gm.extensions["KHR_materials_ior"] = tinygltf::Value(o);
                extsUsed.insert("KHR_materials_ior");
            }
            if (m.emissionStrength != 1.0f)
            {
                tinygltf::Value::Object o;
                o["emissiveStrength"] = tinygltf::Value(static_cast<double>(m.emissionStrength));
                gm.extensions["KHR_materials_emissive_strength"] = tinygltf::Value(o);
                extsUsed.insert("KHR_materials_emissive_strength");
            }

            model.materials.push_back(gm);
            return static_cast<int>(model.materials.size()) - 1;
        }

        bool writeGltf(const Scene &scene, const std::vector<Draw> &draws,
                       const std::string &path, bool binary, std::string *error)
        {
            (void)scene; // geometry/materials come from the prebuilt draw list
            tinygltf::Model model;
            model.asset.version = "2.0";
            model.asset.generator = "traceyv2 SceneExporter";
            model.buffers.emplace_back(); // buffer 0 — everything packs into it

            std::set<std::string> extsUsed;

            // Caches: geometry accessors per SceneObject name; materials by
            // content key; meshes by (object name, material index) so shared
            // geometry stays deduped while per-instance materials are honoured.
            std::unordered_map<std::string, GeoAccessors> geoCache;
            std::unordered_map<std::string, int> matCache;
            std::unordered_map<std::string, int> meshCache;

            tinygltf::Scene gscene;

            for (const auto &d : draws)
            {
                const std::string &objName = d.obj->name();

                auto git = geoCache.find(objName);
                if (git == geoCache.end())
                    git = geoCache.emplace(objName, buildGeoAccessors(model, *d.obj)).first;
                const GeoAccessors &g = git->second;

                ResolvedMaterial rm = resolveMaterial(*d.mat);
                const std::string mkey = materialKey(rm);
                auto mit = matCache.find(mkey);
                if (mit == matCache.end())
                {
                    int idx = buildMaterial(model, extsUsed, rm,
                                            static_cast<int>(model.materials.size()));
                    mit = matCache.emplace(mkey, idx).first;
                }
                const int materialIndex = mit->second;

                const std::string meshKey = objName + "#" + std::to_string(materialIndex);
                auto meshIt = meshCache.find(meshKey);
                if (meshIt == meshCache.end())
                {
                    tinygltf::Primitive prim;
                    prim.mode = TINYGLTF_MODE_TRIANGLES;
                    prim.attributes["POSITION"] = g.position;
                    if (g.normal >= 0) prim.attributes["NORMAL"] = g.normal;
                    if (g.uv >= 0) prim.attributes["TEXCOORD_0"] = g.uv;
                    if (g.color >= 0) prim.attributes["COLOR_0"] = g.color;
                    prim.indices = g.indices;
                    prim.material = materialIndex;

                    tinygltf::Mesh mesh;
                    mesh.name = objName;
                    mesh.primitives.push_back(prim);
                    model.meshes.push_back(mesh);
                    int idx = static_cast<int>(model.meshes.size()) - 1;
                    meshIt = meshCache.emplace(meshKey, idx).first;
                }

                tinygltf::Node node;
                node.mesh = meshIt->second;

                // Emit TRS rather than a matrix: GltfLoader::convertNodeTransform
                // reads node translation/rotation/scale and ignores node.matrix,
                // so TRS is what round-trips through our own importer (and every
                // mainstream glTF reader handles TRS too). Our transforms are
                // clean TRS compositions, so decompose recovers them exactly.
                glm::vec3 scale, translation, skew;
                glm::vec4 perspective;
                glm::quat rotation;
                glm::decompose(d.world, scale, rotation, translation, skew, perspective);
                node.translation = {translation.x, translation.y, translation.z};
                node.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
                node.scale = {scale.x, scale.y, scale.z};

                model.nodes.push_back(node);
                gscene.nodes.push_back(static_cast<int>(model.nodes.size()) - 1);
            }

            model.scenes.push_back(gscene);
            model.defaultScene = 0;
            model.extensionsUsed.assign(extsUsed.begin(), extsUsed.end());

            tinygltf::TinyGLTF ctx;
            bool ok = ctx.WriteGltfSceneToFile(&model, path,
                                               /*embedImages=*/false,
                                               /*embedBuffers=*/true,
                                               /*prettyPrint=*/true,
                                               /*writeBinary=*/binary);
            if (!ok && error)
                *error = "tinygltf failed to write '" + path + "'";
            return ok;
        }

        // =====================================================================
        // OBJ + .mtl  (baked world-space triangles)
        // =====================================================================

        std::string replaceExtension(const std::string &path, const std::string &newExt)
        {
            size_t slash = path.find_last_of("/\\");
            size_t dot = path.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                return path + newExt;
            return path.substr(0, dot) + newExt;
        }

        std::string baseName(const std::string &path)
        {
            size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        bool writeObj(const Scene &scene, const std::vector<Draw> &draws,
                      const std::string &path, std::string *error)
        {
            (void)scene;
            const std::string mtlPath = replaceExtension(path, ".mtl");

            std::ofstream obj(path);
            if (!obj)
            {
                if (error) *error = "could not open '" + path + "' for writing";
                return false;
            }
            std::ofstream mtl(mtlPath);
            if (!mtl)
            {
                if (error) *error = "could not open '" + mtlPath + "' for writing";
                return false;
            }

            obj << "# Exported by traceyv2\n";
            obj << "mtllib " << baseName(mtlPath) << "\n";

            // Material dedup → name; written to the .mtl as we discover them.
            std::unordered_map<std::string, std::string> matNames;

            size_t vOffset = 0, vtOffset = 0, vnOffset = 0;

            for (const auto &d : draws)
            {
                const SceneObject &o = *d.obj;
                const auto &P = o.positions();
                const bool hasN = o.hasNormals() && o.normals().size() == P.size();
                const bool hasUV = o.hasUvs() && o.uvs().size() == P.size();

                // Normals transform by the inverse-transpose of the upper 3x3.
                const Mat3 normalMat = glm::transpose(glm::inverse(Mat3(d.world)));

                // Resolve + register material.
                ResolvedMaterial rm = resolveMaterial(*d.mat);
                const std::string mkey = materialKey(rm);
                auto it = matNames.find(mkey);
                if (it == matNames.end())
                {
                    std::string name = "material_" + std::to_string(matNames.size());
                    it = matNames.emplace(mkey, name).first;
                    const Vec3 ke = rm.emission * rm.emissionStrength;
                    mtl << "newmtl " << name << "\n";
                    mtl << "Kd " << rm.albedo.x << ' ' << rm.albedo.y << ' ' << rm.albedo.z << "\n";
                    mtl << "Ks " << rm.metallic << ' ' << rm.metallic << ' ' << rm.metallic << "\n";
                    mtl << "Ke " << ke.x << ' ' << ke.y << ' ' << ke.z << "\n";
                    mtl << "Ni " << rm.ior << "\n";
                    mtl << "d " << rm.opacity << "\n";
                    mtl << "illum 2\n\n";
                }

                for (const auto &p : P)
                {
                    Vec3 w = transformPoint(d.world, p);
                    obj << "v " << w.x << ' ' << w.y << ' ' << w.z << "\n";
                }
                if (hasUV)
                    for (const auto &uv : o.uvs())
                        obj << "vt " << uv.x << ' ' << uv.y << "\n";
                if (hasN)
                    for (const auto &nrm : o.normals())
                    {
                        Vec3 wn = glm::normalize(normalMat * nrm);
                        obj << "vn " << wn.x << ' ' << wn.y << ' ' << wn.z << "\n";
                    }

                obj << "usemtl " << it->second << "\n";

                std::vector<uint32_t> idx = triangleIndices(o);
                for (size_t t = 0; t + 2 < idx.size(); t += 3)
                {
                    obj << "f";
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint32_t i = idx[t + k];
                        const size_t v = vOffset + i + 1; // OBJ is 1-based
                        obj << ' ' << v;
                        if (hasUV || hasN)
                        {
                            obj << '/';
                            if (hasUV) obj << (vtOffset + i + 1);
                            if (hasN) obj << '/' << (vnOffset + i + 1);
                        }
                    }
                    obj << "\n";
                }

                vOffset += P.size();
                if (hasUV) vtOffset += P.size();
                if (hasN) vnOffset += P.size();
            }

            return true;
        }
    } // namespace

    bool SceneExporter::exportToFile(const Scene &scene, const std::string &path,
                                     Format format, std::string *error)
    {
        const std::vector<Draw> draws = collectDraws(scene);
        if (draws.empty())
        {
            if (error) *error = "scene has no visible geometry to export";
            return false;
        }

        switch (format)
        {
        case Format::GltfJson:
            return writeGltf(scene, draws, path, /*binary=*/false, error);
        case Format::Glb:
            return writeGltf(scene, draws, path, /*binary=*/true, error);
        case Format::Obj:
            return writeObj(scene, draws, path, error);
        }
        if (error) *error = "unknown export format";
        return false;
    }
} // namespace tracey
