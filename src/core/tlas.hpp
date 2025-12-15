#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "hit.hpp"
#include "bvh_node.hpp"
#include "ray.hpp"
namespace tracey
{
    class Blas;
    class Tlas
    {
    public:
        struct Instance
        {
            float transform[3][4]; // row-major 3x4 matrix
            // Default mask = 0xFF so instances are visible unless the caller overrides it.
            uint32_t instanceCustomIndexAndMask = 0;
            uint32_t instanceShaderBindingTableRecordOffsetAndFlags = 0u;
            uint64_t blasAddress = 0; // Address of the BLAS this instance refers to

            Instance()
            {
                // Initialize to identity matrix
                transform[0][0] = 1.0f;
                transform[0][1] = 0.0f;
                transform[0][2] = 0.0f;
                transform[0][3] = 0.0f;
                transform[1][0] = 0.0f;
                transform[1][1] = 1.0f;
                transform[1][2] = 0.0f;
                transform[1][3] = 0.0f;
                transform[2][0] = 0.0f;
                transform[2][1] = 0.0f;
                transform[2][2] = 1.0f;
                transform[2][3] = 0.0f;
            }

            void setTransformRowMajor3x4(const float mat[3][4])
            {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        transform[r][c] = mat[r][c];
            }

            void setTransform(const Mat4 &mat)
            {
                // GLM Mat4 is column-major: mat[col][row]
                // VkTransformMatrixKHR is row-major 3x4: transform[row][col]
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        transform[r][c] = mat[c][r];
            }

            uint32_t instanceCustomIndex() const
            {
                return instanceCustomIndexAndMask & 0xFFFFFF;
            }

            uint32_t instanceMask() const
            {
                return (instanceCustomIndexAndMask >> 24) & 0xFF;
            }

            void setCustomIndex(uint32_t index)
            {
                instanceCustomIndexAndMask = (instanceCustomIndexAndMask & 0xFF000000) | (index & 0xFFFFFF);
            }

            void setMask(uint32_t mask)
            {
                instanceCustomIndexAndMask = (instanceCustomIndexAndMask & 0x00FFFFFF) | ((mask & 0xFF) << 24);
            }

            uint32_t sbtRecordOffset() const
            {
                return instanceShaderBindingTableRecordOffsetAndFlags & 0xFFFFFF;
            }

            uint32_t instanceFlags() const
            {
                return (instanceShaderBindingTableRecordOffsetAndFlags >> 24) & 0xFF;
            }

            void setSbtRecordOffset(uint32_t offset)
            {
                instanceShaderBindingTableRecordOffsetAndFlags =
                    (instanceShaderBindingTableRecordOffsetAndFlags & 0xFF000000) | (offset & 0xFFFFFF);
            }

            void setInstanceFlags(uint32_t flags)
            {
                instanceShaderBindingTableRecordOffsetAndFlags =
                    (instanceShaderBindingTableRecordOffsetAndFlags & 0x00FFFFFF) | ((flags & 0xFF) << 24);
            }

            Vec3 transformPoint(const Vec3 &p) const
            {
                return {
                    transform[0][0] * p.x + transform[0][1] * p.y + transform[0][2] * p.z + transform[0][3],
                    transform[1][0] * p.x + transform[1][1] * p.y + transform[1][2] * p.z + transform[1][3],
                    transform[2][0] * p.x + transform[2][1] * p.y + transform[2][2] * p.z + transform[2][3],
                };
            }

            Vec3 transformVector(const Vec3 &v) const
            {
                return {
                    transform[0][0] * v.x + transform[0][1] * v.y + transform[0][2] * v.z,
                    transform[1][0] * v.x + transform[1][1] * v.y + transform[1][2] * v.z,
                    transform[2][0] * v.x + transform[2][1] * v.y + transform[2][2] * v.z,
                };
            }
        };

        Tlas(std::span<const Blas *> blases, std::span<const Instance> instances);

        std::optional<Hit> intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const;

    private:
        std::span<const Blas *> blases;
        std::span<const Instance> instances;
        std::vector<BVHNode> m_nodes;
    };
}