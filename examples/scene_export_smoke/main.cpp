// Smoke test for SceneExporter (glTF / GLB / OBJ geometry export).
//
// Builds a small scene by hand — a glass cube (transmission/ior/opacity/
// emission) offset on +X, and a 3-instance sphere actor — then:
//   1. Exports glTF, GLB, OBJ and asserts each file (and the .mtl sidecar)
//      was written non-empty.
//   2. Re-imports the glTF through GltfLoader and asserts:
//        - triangle count is preserved (geometry survived),
//        - the world-space bounding box matches the source within tolerance
//          (per-instance TRS transforms survived — catches a matrix-vs-TRS
//          export regression, which would collapse everything to the origin),
//        - the glass material's PBR factors round-trip (albedo, metallic,
//          roughness, transmission, ior, opacity, emission, emissive strength).
//   3. Parses the OBJ and asserts the baked world-space vertex count equals
//      the sum over instances and that it references the .mtl.
//
// Exit 0 on success. Depends only on `tracey` — no Vulkan, no rendering.

#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/scene_instance.hpp"
#include "scene/material_instance.hpp"
#include "scene/actor.hpp"
#include "scene/transform.hpp"
#include "scene/scene_exporter.hpp"
#include "scene/gltf_loader.hpp"
#include "core/types.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tracey;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}

bool approx(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

struct AABB
{
    Vec3 lo{0}, hi{0};
    bool valid = false;
    void add(const Vec3 &p)
    {
        if (!valid) { lo = hi = p; valid = true; }
        else { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    }
};

AABB worldAabb(const Scene &s)
{
    AABB b;
    for (const auto &node : s.flatten())
    {
        const Actor *a = node.actor;
        if (!a || !a->visible()) continue;
        for (const auto &inst : a->instances())
        {
            const SceneObject *o = s.getObject(inst.objectRef());
            if (!o) continue;
            Mat4 w = node.worldTransform;
            if (inst.hasLocalTransform()) w = w * inst.localTransform()->toMatrix();
            for (const auto &p : o->positions()) b.add(transformPoint(w, p));
        }
    }
    return b;
}

size_t sceneTriangleCount(const Scene &s)
{
    size_t n = 0;
    for (const auto &node : s.flatten())
    {
        const Actor *a = node.actor;
        if (!a || !a->visible()) continue;
        for (const auto &inst : a->instances())
        {
            const SceneObject *o = s.getObject(inst.objectRef());
            if (!o) continue;
            size_t idx = o->indices().empty() ? o->positions().size() : o->indices().size();
            n += idx / 3;
        }
    }
    return n;
}

const MaterialInstance *findGlass(const Scene &s)
{
    for (const auto *a : s.actors())
        for (const auto &inst : a->instances())
        {
            auto t = inst.material().getFloat("transmission");
            if (t && std::fabs(*t - 0.9f) < 0.05f) return &inst.material();
        }
    return nullptr;
}

} // namespace

int main()
{
    std::printf("[scene_export_smoke]\n");

    // ── Build the source scene ──────────────────────────────────────────
    Scene scene;
    scene.addObject("cube", SceneObject::createCube(1.0f));
    scene.addObject("sphere", SceneObject::createSphere(1.0f, 16, 16));

    const size_t cubeVerts = scene.getObject("cube")->positions().size();
    const size_t sphereVerts = scene.getObject("sphere")->positions().size();

    // Glass cube, offset on +X via the actor's local transform.
    MaterialInstance glass("pbr");
    glass.setAlbedo(Vec3(0.2f, 0.8f, 0.9f));
    glass.setMetallic(0.0f);
    glass.setRoughness(0.1f);
    glass.setEmission(Vec3(1.0f, 0.4f, 0.0f));
    glass.setFloat("transmission", 0.9f);
    glass.setFloat("ior", 1.45f);
    glass.setFloat("opacity", 0.5f);
    glass.setFloat("emissionStrength", 3.0f);
    {
        Actor *a = scene.createActor();
        a->setName("glassCube");
        Transform t;
        t.setPosition(Vec3(5.0f, 0.0f, 0.0f));
        a->setTransform(t);
        a->addInstance(SceneInstance("cube", glass));
    }

    // Three sphere instances at distinct positions (per-instance transforms).
    {
        Actor *a = scene.createActor();
        a->setName("spheres");
        MaterialInstance mat("pbr");
        mat.setAlbedo(Vec3(0.8f, 0.2f, 0.2f));
        mat.setMetallic(0.0f);
        mat.setRoughness(0.5f);
        const Vec3 offsets[3] = {{-4.f, 0.f, 0.f}, {0.f, 3.f, 0.f}, {0.f, 0.f, -6.f}};
        for (const auto &off : offsets)
        {
            SceneInstance inst("sphere", mat);
            Transform lt;
            lt.setPosition(off);
            inst.setLocalTransform(lt);
            a->addInstance(std::move(inst));
        }
    }

    const size_t srcTris = sceneTriangleCount(scene);
    const AABB srcBox = worldAabb(scene);

    // ── Export ──────────────────────────────────────────────────────────
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tracey_export_smoke";
    fs::create_directories(dir);
    const std::string gltfPath = (dir / "scene.gltf").string();
    const std::string glbPath = (dir / "scene.glb").string();
    const std::string objPath = (dir / "scene.obj").string();
    const std::string mtlPath = (dir / "scene.mtl").string();

    std::string err;
    check(SceneExporter::exportToFile(scene, gltfPath, SceneExporter::Format::GltfJson, &err),
          ("export glTF" + (err.empty() ? "" : " (" + err + ")")).c_str());
    err.clear();
    check(SceneExporter::exportToFile(scene, glbPath, SceneExporter::Format::Glb, &err),
          ("export GLB" + (err.empty() ? "" : " (" + err + ")")).c_str());
    err.clear();
    check(SceneExporter::exportToFile(scene, objPath, SceneExporter::Format::Obj, &err),
          ("export OBJ" + (err.empty() ? "" : " (" + err + ")")).c_str());

    auto fileSize = [](const std::string &p) -> uintmax_t {
        std::error_code ec;
        auto s = fs::file_size(p, ec);
        return ec ? 0 : s;
    };
    check(fileSize(gltfPath) > 0, "glTF non-empty");
    check(fileSize(glbPath) > 0, "GLB non-empty");
    check(fileSize(objPath) > 0, "OBJ non-empty");
    check(fileSize(mtlPath) > 0, "MTL sidecar non-empty");

    // ── Re-import the glTF and validate the round-trip ──────────────────
    auto reimported = GltfLoader::loadFromFile(gltfPath);
    check(reimported != nullptr, "re-import glTF");
    if (reimported)
    {
        const size_t rtTris = sceneTriangleCount(*reimported);
        check(rtTris == srcTris, "triangle count preserved");
        if (rtTris != srcTris)
            std::printf("       (source=%zu reimport=%zu)\n", srcTris, rtTris);

        const AABB rtBox = worldAabb(*reimported);
        const bool boxOk = rtBox.valid && srcBox.valid &&
                           approx(rtBox.lo.x, srcBox.lo.x, 1e-2f) &&
                           approx(rtBox.lo.y, srcBox.lo.y, 1e-2f) &&
                           approx(rtBox.lo.z, srcBox.lo.z, 1e-2f) &&
                           approx(rtBox.hi.x, srcBox.hi.x, 1e-2f) &&
                           approx(rtBox.hi.y, srcBox.hi.y, 1e-2f) &&
                           approx(rtBox.hi.z, srcBox.hi.z, 1e-2f);
        check(boxOk, "world-space bounds preserved (transforms survived)");
        if (!boxOk)
            std::printf("       src=[%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f] rt=[%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f]\n",
                        srcBox.lo.x, srcBox.lo.y, srcBox.lo.z, srcBox.hi.x, srcBox.hi.y, srcBox.hi.z,
                        rtBox.lo.x, rtBox.lo.y, rtBox.lo.z, rtBox.hi.x, rtBox.hi.y, rtBox.hi.z);

        const MaterialInstance *m = findGlass(*reimported);
        check(m != nullptr, "glass material located after re-import");
        if (m)
        {
            auto alb = m->albedo().value_or(Vec3(0));
            check(approx(alb.x, 0.2f) && approx(alb.y, 0.8f) && approx(alb.z, 0.9f),
                  "albedo round-trip");
            check(approx(m->metallic().value_or(-1.f), 0.0f), "metallic round-trip");
            check(approx(m->roughness().value_or(-1.f), 0.1f), "roughness round-trip");
            check(approx(m->getFloat("transmission").value_or(-1.f), 0.9f),
                  "transmission round-trip");
            check(approx(m->getFloat("ior").value_or(-1.f), 1.45f), "ior round-trip");
            check(approx(m->getFloat("opacity").value_or(-1.f), 0.5f), "opacity round-trip");
            auto em = m->emission().value_or(Vec3(-1));
            check(approx(em.x, 1.0f) && approx(em.y, 0.4f) && approx(em.z, 0.0f),
                  "emission round-trip");
            check(approx(m->getFloat("emissionStrength").value_or(-1.f), 3.0f),
                  "emission strength round-trip");
        }
    }

    // ── Validate the OBJ output ─────────────────────────────────────────
    {
        std::ifstream obj(objPath);
        size_t vCount = 0;
        bool sawMtllib = false, sawUsemtl = false, sawFace = false;
        std::string line;
        while (std::getline(obj, line))
        {
            if (line.rfind("v ", 0) == 0) ++vCount;
            else if (line.rfind("mtllib", 0) == 0) sawMtllib = true;
            else if (line.rfind("usemtl", 0) == 0) sawUsemtl = true;
            else if (line.rfind("f ", 0) == 0) sawFace = true;
        }
        const size_t expectedV = cubeVerts + 3 * sphereVerts;
        check(vCount == expectedV, "OBJ baked vertex count");
        if (vCount != expectedV)
            std::printf("       (expected=%zu got=%zu)\n", expectedV, vCount);
        check(sawMtllib, "OBJ references .mtl");
        check(sawUsemtl, "OBJ emits usemtl");
        check(sawFace, "OBJ emits faces");

        std::ifstream mtl(mtlPath);
        std::string mtlText((std::istreambuf_iterator<char>(mtl)),
                            std::istreambuf_iterator<char>());
        check(mtlText.find("newmtl") != std::string::npos, "MTL defines a material");
    }

    // ── glTF node.matrix import (regression guard) ──────────────────────
    // Hand-author a minimal glTF whose single node carries a transform as a
    // 4x4 `matrix` (not TRS) = translate(7,8,9) · rotateZ(90°), referencing a
    // 3-vertex mesh via an external .bin. The importer must decompose the
    // matrix — previously it ignored it and parked the node at the origin.
    {
        const std::string binPath = (dir / "tri.bin").string();
        const std::string mtxGltf = (dir / "matrix_node.gltf").string();
        const float verts[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
        {
            std::ofstream bin(binPath, std::ios::binary);
            bin.write(reinterpret_cast<const char *>(verts), sizeof(verts));
        }
        // Column-major translate(7,8,9)·rotateZ(90°). Maps local +X (1,0,0)
        // to (0,1,0) then offsets → world (7,9,9).
        std::ofstream g(mtxGltf);
        g << R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "matrix": [0,1,0,0, -1,0,0,0, 0,0,1,0, 7,8,9,1]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": 4}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962}],
  "buffers": [{"uri": "tri.bin", "byteLength": 36}]
})";
        g.close();

        auto mi = GltfLoader::loadFromFile(mtxGltf);
        check(mi != nullptr, "load glTF with node.matrix");
        if (mi)
        {
            auto nodes = mi->flatten();
            check(nodes.size() == 1, "matrix-node glTF flattens to one node");
            if (!nodes.empty())
            {
                const Vec3 world = transformPoint(nodes[0].worldTransform, Vec3(1.f, 0.f, 0.f));
                const bool ok = approx(world.x, 7.f, 1e-2f) &&
                                approx(world.y, 9.f, 1e-2f) &&
                                approx(world.z, 9.f, 1e-2f);
                check(ok, "node.matrix decomposed (translation + rotation applied)");
                if (!ok)
                    std::printf("       expected (7,9,9) got (%.3f,%.3f,%.3f)\n",
                                world.x, world.y, world.z);
            }
        }
    }

    // ── TRS rotation round-trip (exporter decompose + importer TRS path) ──
    // Export a rotated actor (our exporter writes TRS), re-import, and verify a
    // known local point lands where the source transform put it — guards both
    // the exporter's matrix→TRS decompose and the importer's quaternion path.
    {
        Scene rs;
        rs.addObject("marker", SceneObject::createCube(1.0f));
        Actor *a = rs.createActor();
        a->setName("rotated");
        Transform t;
        t.setRotation(glm::angleAxis(glm::radians(90.0f), Vec3(0.f, 0.f, 1.f)));
        t.setPosition(Vec3(2.f, 0.f, 0.f));
        a->setTransform(t);
        a->addInstance(SceneInstance("marker", MaterialInstance("pbr")));

        const Vec3 localPt(0.5f, 0.f, 0.f);
        const Vec3 srcWorld = transformPoint(t.toMatrix(), localPt); // (2, 0.5, 0)

        const std::string rotPath = (dir / "rotated.gltf").string();
        std::string e2;
        check(SceneExporter::exportToFile(rs, rotPath, SceneExporter::Format::GltfJson, &e2),
              "export rotated scene");
        auto ri = GltfLoader::loadFromFile(rotPath);
        check(ri != nullptr, "re-import rotated scene");
        if (ri)
        {
            auto nodes = ri->flatten();
            if (!nodes.empty())
            {
                const Vec3 rtWorld = transformPoint(nodes[0].worldTransform, localPt);
                const bool ok = approx(rtWorld.x, srcWorld.x, 1e-2f) &&
                                approx(rtWorld.y, srcWorld.y, 1e-2f) &&
                                approx(rtWorld.z, srcWorld.z, 1e-2f);
                check(ok, "rotation round-trip through TRS export/import");
                if (!ok)
                    std::printf("       src=(%.3f,%.3f,%.3f) rt=(%.3f,%.3f,%.3f)\n",
                                srcWorld.x, srcWorld.y, srcWorld.z, rtWorld.x, rtWorld.y, rtWorld.z);
            }
        }
    }

    std::printf(failures == 0 ? "[scene_export_smoke] all checks passed\n"
                              : "[scene_export_smoke] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
