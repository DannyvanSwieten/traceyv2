#include "geometry_converter.hpp"

#include "../scene/scene_object.hpp"

namespace tracey
{
    SceneObject GeometryConverter::toSceneObject(const Geometry &geo, const std::string &name)
    {
        SceneObject out(name);

        const auto &v2p = geo.vertexToPoint();
        const auto *P = geo.points().get<Vec3>("P");
        if (!P) return out; // empty

        // Walk primitives, expanding to triangles (only triangle prims for v1).
        const auto &prims = geo.primitivesList();

        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;
        std::vector<Vec3> colors;

        const auto *vertexN = geo.vertices().get<Vec3>("N");
        const auto *pointN = geo.points().get<Vec3>("N");
        const auto *vertexUV = geo.vertices().get<Vec2>("uv");
        const auto *pointUV = geo.points().get<Vec2>("uv");
        // Vertex color "Cd" — the VOP geo_output.Cd port emits it as a
        // Point attribute; we also honour a Vertex-class Cd for per-corner
        // shading when present.
        const auto *vertexCd = geo.vertices().get<Vec3>("Cd");
        const auto *pointCd = geo.points().get<Vec3>("Cd");

        const bool wantNormals = vertexN || pointN;
        const bool wantUVs = vertexUV || pointUV;
        const bool wantColors = vertexCd || pointCd;

        size_t triCount = 0;
        for (const GeoPrimitive &p : prims) if (p.vertexCount == 3) ++triCount;

        positions.reserve(triCount * 3);
        if (wantNormals) normals.reserve(triCount * 3);
        if (wantUVs) uvs.reserve(triCount * 3);
        if (wantColors) colors.reserve(triCount * 3);

        for (const GeoPrimitive &p : prims)
        {
            if (p.vertexCount != 3) continue; // skip non-triangles for v1
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t vid = p.firstVertex + k;
                const uint32_t pid = v2p[vid];

                positions.push_back(P->at(pid));

                if (wantNormals)
                {
                    if (vertexN) normals.push_back(vertexN->at(vid));
                    else         normals.push_back(pointN->at(pid));
                }

                if (wantUVs)
                {
                    if (vertexUV) uvs.push_back(vertexUV->at(vid));
                    else          uvs.push_back(pointUV->at(pid));
                }

                if (wantColors)
                {
                    if (vertexCd) colors.push_back(vertexCd->at(vid));
                    else          colors.push_back(pointCd->at(pid));
                }
            }
        }

        // Point-cloud fallback: when the geometry has points but no triangle
        // primitives (e.g. scatter, points_grid emitting a bare point cloud),
        // emit each point as a degenerate triangle — three copies of the same
        // position. The BLAS builds cleanly (path tracer rays harmlessly miss
        // zero-area triangles) and the rasterizer's points pipeline finds a
        // vertex buffer to drive its POINT_LIST splat draws. Without this,
        // scatter output renders nothing — the cube etc. test against
        // `triCount > 0` and the SceneCompiler skipped any object whose
        // positions array was empty.
        if (triCount == 0 && positions.empty() && P->data().size() > 0)
        {
            const size_t pCount = P->data().size();
            positions.reserve(pCount * 3);
            if (wantNormals) normals.reserve(pCount * 3);
            if (wantUVs)     uvs.reserve(pCount * 3);
            if (wantColors)  colors.reserve(pCount * 3);
            for (size_t pid = 0; pid < pCount; ++pid)
            {
                const Vec3 pos = P->at(pid);
                positions.push_back(pos);
                positions.push_back(pos);
                positions.push_back(pos);

                if (wantNormals)
                {
                    const Vec3 n = pointN ? pointN->at(pid)
                                          : Vec3(0.0f, 1.0f, 0.0f);
                    normals.push_back(n);
                    normals.push_back(n);
                    normals.push_back(n);
                }
                if (wantUVs)
                {
                    const Vec2 u = pointUV ? pointUV->at(pid) : Vec2(0.0f);
                    uvs.push_back(u);
                    uvs.push_back(u);
                    uvs.push_back(u);
                }
                if (wantColors)
                {
                    const Vec3 c = pointCd ? pointCd->at(pid) : Vec3(1.0f);
                    colors.push_back(c);
                    colors.push_back(c);
                    colors.push_back(c);
                }
            }
        }

        out.setPositions(std::move(positions));
        if (wantNormals) out.setNormals(std::move(normals));
        if (wantUVs) out.setUvs(std::move(uvs));
        if (wantColors) out.setColors(std::move(colors));
        // Leave indices empty — per-corner expansion matches the existing
        // primitive generators' format.
        return out;
    }

    Geometry GeometryConverter::fromSceneObject(const SceneObject &obj)
    {
        Geometry geo;
        const auto &positions = obj.positions();
        if (positions.empty()) return geo;

        const bool indexed = obj.hasIndices();
        const bool hasNormals = obj.hasNormals();
        const bool hasUVs = obj.hasUvs();

        // For each face corner we emit one point + one vertex (no welding).
        // This matches how the existing primitive generators ship per-corner
        // unique vertices. Welding can come later as a Fuse SOP.

        // Pre-compute the corner count for sizing.
        const size_t cornerCount = indexed ? obj.indices().size() : positions.size();
        const size_t triCount = cornerCount / 3;

        // Pre-add attributes so resize() distributes defaults correctly.
        Attribute<Vec3> *N = hasNormals
            ? geo.points().add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f))
            : nullptr;
        Attribute<Vec2> *uv = hasUVs
            ? geo.vertices().add<Vec2>("uv", Vec2(0.0f))
            : nullptr;

        // Reserve point/vertex storage.
        geo.points().resize(cornerCount);
        geo.vertices().resize(cornerCount);
        geo.vertexToPoint().resize(cornerCount);

        auto *P = geo.points().get<Vec3>("P");

        for (size_t c = 0; c < cornerCount; ++c)
        {
            const uint32_t srcIndex = indexed
                ? obj.indices()[c]
                : static_cast<uint32_t>(c);

            P->at(c) = positions[srcIndex];
            geo.vertexToPoint()[c] = static_cast<uint32_t>(c);

            if (N) N->at(c) = obj.normals()[srcIndex];
            if (uv) uv->at(c) = obj.uvs()[srcIndex];
        }

        // Emit triangle primitives.
        geo.primitivesList().reserve(triCount);
        for (size_t t = 0; t < triCount; ++t)
        {
            geo.primitivesList().push_back({static_cast<uint32_t>(t * 3), 3u});
        }
        geo.primitives().resize(triCount);
        return geo;
    }
}
