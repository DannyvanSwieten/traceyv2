#include "geometry.hpp"
#include "scene_object.hpp"

namespace tracey
{
    // Use SceneObject's factory methods and extract the geometry data
    Geometry Geometry::createCube(float size)
    {
        auto obj = SceneObject::createCube(size);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    Geometry Geometry::createPlane(float width, float depth)
    {
        auto obj = SceneObject::createPlane(width, depth);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    Geometry Geometry::createSphere(float radius, uint32_t segments, uint32_t rings)
    {
        auto obj = SceneObject::createSphere(radius, segments, rings);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    Geometry Geometry::createTorus(float majorRadius, float minorRadius,
                                   uint32_t majorSegments, uint32_t minorSegments)
    {
        auto obj = SceneObject::createTorus(majorRadius, minorRadius, majorSegments, minorSegments);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    Geometry Geometry::createCylinder(float radius, float height, uint32_t segments)
    {
        auto obj = SceneObject::createCylinder(radius, height, segments);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    Geometry Geometry::createCone(float radius, float height, uint32_t segments)
    {
        auto obj = SceneObject::createCone(radius, height, segments);
        Geometry geom;
        geom.setPositions(obj.positions());
        geom.setIndices(obj.indices());
        geom.setNormals(obj.normals());
        geom.setUvs(obj.uvs());
        return geom;
    }

    SceneObject Geometry::toSceneObject(const std::string& name) const
    {
        SceneObject obj(name);
        obj.setPositions(m_positions);
        obj.setIndices(m_indices);
        obj.setNormals(m_normals);
        obj.setUvs(m_uvs);
        // Note: Attributes are not copied - SceneObject uses its own AttributeSet
        return obj;
    }

} // namespace tracey
