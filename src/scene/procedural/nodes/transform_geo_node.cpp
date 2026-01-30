#include "transform_geo_node.hpp"
#include "../node_graph.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

namespace tracey
{
    TransformGeoNode::TransformGeoNode(size_t uid, std::string name)
        : ProceduralNode(uid, NodeType::GeometryTransform, std::move(name))
    {
        initializeParameters();
    }

    void TransformGeoNode::initializeParameters()
    {
        // Translation parameter
        addParameter(std::make_unique<Parameter>(
            "translate",
            ParameterType::Vec3,
            Vec3(0.0f, 0.0f, 0.0f)
        ));

        // Rotation parameter (euler angles in degrees)
        addParameter(std::make_unique<Parameter>(
            "rotate",
            ParameterType::Vec3,
            Vec3(0.0f, 0.0f, 0.0f)
        ));

        // Non-uniform scale parameter
        addParameter(std::make_unique<Parameter>(
            "scale",
            ParameterType::Vec3,
            Vec3(1.0f, 1.0f, 1.0f)
        ));

        // Uniform scale parameter (multiplier)
        addParameter(std::make_unique<Parameter>(
            "uniformScale",
            ParameterType::Float,
            1.0f
        ));
    }

    NodeEvaluationResult TransformGeoNode::evaluate(const EvaluationContext& ctx)
    {
        NodeEvaluationResult result;

        try {
            // Find input geometry from connections
            std::shared_ptr<Geometry> inputGeometry;

            if (ctx.graph) {
                for (const auto& conn : ctx.graph->connections()) {
                    if (conn.toNode == uid()) {
                        // Get the source node's cached result
                        auto cacheIt = ctx.cache.find(conn.fromNode);
                        if (cacheIt != ctx.cache.end()) {
                            const auto& nodeResult = cacheIt->second;
                            // Try to extract Geometry from the result
                            if (auto* geomPtr = std::get_if<std::shared_ptr<Geometry>>(&nodeResult.data)) {
                                inputGeometry = *geomPtr;
                                break; // Only use first input
                            }
                        }
                    }
                }
            }

            // If no input, return empty geometry
            if (!inputGeometry) {
                result.data = std::make_shared<Geometry>();
                result.success = true;
                return result;
            }

            // Get transformation parameters
            Vec3 translate(0.0f, 0.0f, 0.0f);
            Vec3 rotate(0.0f, 0.0f, 0.0f);
            Vec3 scale(1.0f, 1.0f, 1.0f);
            float uniformScale = 1.0f;

            if (auto* param = getParameter("translate")) {
                if (auto* val = getValuePtr<Vec3>(param->value())) {
                    translate = *val;
                }
            }

            if (auto* param = getParameter("rotate")) {
                if (auto* val = getValuePtr<Vec3>(param->value())) {
                    rotate = *val;
                }
            }

            if (auto* param = getParameter("scale")) {
                if (auto* val = getValuePtr<Vec3>(param->value())) {
                    scale = *val;
                }
            }

            if (auto* param = getParameter("uniformScale")) {
                if (auto* val = getValuePtr<float>(param->value())) {
                    uniformScale = *val;
                }
            }

            // Build transformation matrix
            // Order: Scale -> Rotate -> Translate
            glm::mat4 transform(1.0f);

            // Apply translation
            transform = glm::translate(transform, glm::vec3(translate.x, translate.y, translate.z));

            // Apply rotation (convert degrees to radians)
            float radX = glm::radians(rotate.x);
            float radY = glm::radians(rotate.y);
            float radZ = glm::radians(rotate.z);
            transform = transform * glm::eulerAngleXYZ(radX, radY, radZ);

            // Apply scale
            glm::vec3 finalScale = glm::vec3(scale.x * uniformScale, scale.y * uniformScale, scale.z * uniformScale);
            transform = glm::scale(transform, finalScale);

            // Create output geometry by copying input
            Geometry outputGeom;
            outputGeom.setIndices(inputGeometry->indices());
            outputGeom.setUvs(inputGeometry->uvs());

            // Transform positions
            const auto& inputPositions = inputGeometry->positions();
            std::vector<Vec3> outputPositions;
            outputPositions.reserve(inputPositions.size());

            for (const auto& pos : inputPositions) {
                glm::vec4 p(pos.x, pos.y, pos.z, 1.0f);
                glm::vec4 transformed = transform * p;
                outputPositions.push_back(Vec3(transformed.x, transformed.y, transformed.z));
            }
            outputGeom.setPositions(std::move(outputPositions));

            // Transform normals (rotation only, no translation or scale)
            if (inputGeometry->hasNormals()) {
                // Normal matrix is the inverse transpose of the rotation part
                glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(transform)));

                const auto& inputNormals = inputGeometry->normals();
                std::vector<Vec3> outputNormals;
                outputNormals.reserve(inputNormals.size());

                for (const auto& normal : inputNormals) {
                    glm::vec3 n(normal.x, normal.y, normal.z);
                    glm::vec3 transformed = glm::normalize(normalMatrix * n);
                    outputNormals.push_back(Vec3(transformed.x, transformed.y, transformed.z));
                }
                outputGeom.setNormals(std::move(outputNormals));
            }

            result.data = std::make_shared<Geometry>(std::move(outputGeom));
            result.success = true;

        } catch (const std::exception& e) {
            result.success = false;
            result.error = std::string("Transform node failed: ") + e.what();
        }

        return result;
    }

} // namespace tracey
