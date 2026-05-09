#include "geometry.hpp"

#include <stdexcept>

namespace tracey
{
    Geometry::Geometry()
        : m_pointAttrs(AttributeClass::Point),
          m_vertexAttrs(AttributeClass::Vertex),
          m_primAttrs(AttributeClass::Primitive),
          m_detailAttrs(AttributeClass::Detail)
    {
        // Mandatory attributes:
        //   P (point/Vec3): position. Always exists; SOPs and the
        //                   converter rely on it.
        m_pointAttrs.add<Vec3>("P", Vec3(0.0f));
        m_detailAttrs.resize(1);
    }

    size_t Geometry::addPoint(const Vec3 &p)
    {
        const size_t id = m_pointAttrs.size();
        m_pointAttrs.resize(id + 1);
        // Write P after resize so it isn't clobbered by the default fill.
        auto *P = m_pointAttrs.get<Vec3>("P");
        if (P) P->at(id) = p;
        return id;
    }

    size_t Geometry::addTriangle(uint32_t p0, uint32_t p1, uint32_t p2)
    {
        const uint32_t firstVertex = static_cast<uint32_t>(m_vertexAttrs.size());

        m_vertexToPoint.push_back(p0);
        m_vertexToPoint.push_back(p1);
        m_vertexToPoint.push_back(p2);
        m_vertexAttrs.resize(firstVertex + 3);

        const size_t primId = m_primitivesList.size();
        m_primitivesList.push_back({firstVertex, 3u});
        m_primAttrs.resize(primId + 1);
        return primId;
    }

    namespace
    {
        // Append `src` attributes into `dst`, offsetting `dst` indices below
        // `dstOriginalSize` to leave them untouched. Only attributes whose
        // (name, type) match in dst receive data; the rest are ignored.
        template <typename T>
        void copy_attribute_typed(const Attribute<T> *srcAttr, Attribute<T> *dstAttr,
                                  size_t dstStart)
        {
            const auto &src = srcAttr->data();
            auto &dst = dstAttr->data();
            for (size_t i = 0; i < src.size(); ++i)
            {
                dst[dstStart + i] = src[i];
            }
        }

        // Try every supported element type until one matches.
        void copy_attribute(const AttributeBase *src, AttributeBase *dst, size_t dstStart)
        {
            if (src->typeIndex() != dst->typeIndex()) return;
#define TRY_TYPE(T)                                                                  \
    if (auto *s = dynamic_cast<const Attribute<T> *>(src);                           \
        s && dynamic_cast<Attribute<T> *>(dst))                                      \
    {                                                                                \
        copy_attribute_typed<T>(s, dynamic_cast<Attribute<T> *>(dst), dstStart);     \
        return;                                                                      \
    }
            TRY_TYPE(float)
            TRY_TYPE(int)
            TRY_TYPE(Vec2)
            TRY_TYPE(Vec3)
            TRY_TYPE(Vec4)
            TRY_TYPE(Mat3)
            TRY_TYPE(Mat4)
            TRY_TYPE(std::string)
#undef TRY_TYPE
        }
    } // anon

    void Geometry::mergeFrom(const Geometry &other)
    {
        const size_t pointBase = m_pointAttrs.size();
        const size_t vertexBase = m_vertexAttrs.size();
        const size_t primBase = m_primAttrs.size();

        // Resize to accommodate other's contents first; new slots get
        // defaults, then we copy other's data into them.
        m_pointAttrs.resize(pointBase + other.m_pointAttrs.size());
        m_vertexAttrs.resize(vertexBase + other.m_vertexAttrs.size());
        m_primAttrs.resize(primBase + other.m_primAttrs.size());

        // Vertex→point: copy with point id offset.
        m_vertexToPoint.reserve(m_vertexToPoint.size() + other.m_vertexToPoint.size());
        for (uint32_t v : other.m_vertexToPoint)
        {
            m_vertexToPoint.push_back(v + static_cast<uint32_t>(pointBase));
        }

        // Primitives: copy with vertex offset.
        m_primitivesList.reserve(m_primitivesList.size() + other.m_primitivesList.size());
        for (const GeoPrimitive &p : other.m_primitivesList)
        {
            m_primitivesList.push_back({p.firstVertex + static_cast<uint32_t>(vertexBase),
                                        p.vertexCount});
        }

        // Attribute payloads: only merge attributes that exist on the dst
        // side (matching name + type). New attributes from other are ignored
        // — caller can pre-add them on this side if they want them preserved.
        for (const auto &name : other.m_pointAttrs.names())
        {
            const auto *src = other.m_pointAttrs.find(name);
            auto *dst = m_pointAttrs.find(name);
            if (src && dst) copy_attribute(src, dst, pointBase);
        }
        for (const auto &name : other.m_vertexAttrs.names())
        {
            const auto *src = other.m_vertexAttrs.find(name);
            auto *dst = m_vertexAttrs.find(name);
            if (src && dst) copy_attribute(src, dst, vertexBase);
        }
        for (const auto &name : other.m_primAttrs.names())
        {
            const auto *src = other.m_primAttrs.find(name);
            auto *dst = m_primAttrs.find(name);
            if (src && dst) copy_attribute(src, dst, primBase);
        }
    }

    std::vector<Vec3> &Geometry::positions()
    {
        auto *P = m_pointAttrs.get<Vec3>("P");
        if (!P) throw std::runtime_error("Geometry: P attribute missing");
        return P->data();
    }

    const std::vector<Vec3> &Geometry::positions() const
    {
        const auto *P = m_pointAttrs.get<Vec3>("P");
        if (!P) throw std::runtime_error("Geometry: P attribute missing");
        return P->data();
    }
}
