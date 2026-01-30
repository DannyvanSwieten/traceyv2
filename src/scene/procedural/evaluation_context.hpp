#pragma once

#include "parameter_value.hpp"
#include <unordered_map>
#include <cstddef>
#include <memory>

namespace tracey
{
    // Forward declarations
    class NodeGraph;
    class SceneObject;

    /**
     * @brief Result of evaluating a single node
     *
     * Contains the output data from a node's evaluate() method.
     * The variant holds different types of data depending on the node type:
     * - Geometry nodes output SceneObject
     * - Material nodes output MaterialInstance
     * - Math nodes output scalars/vectors
     * - Transform nodes output transformation data
     */
    struct NodeEvaluationResult
    {
        std::variant<
            std::monostate,                            // No output / void
            std::shared_ptr<SceneObject>,             // Geometry output
            float,                                     // Scalar output
            Vec3,                                      // Vector output
            Vec4,                                      // Vector4 output
            std::vector<std::shared_ptr<SceneObject>> // Multi-geometry output
        > data;

        bool success = true;
        std::string error;

        NodeEvaluationResult() = default;

        // Constructors for convenience
        explicit NodeEvaluationResult(std::shared_ptr<SceneObject> geometry)
            : data(std::move(geometry)), success(true) {}

        explicit NodeEvaluationResult(float value)
            : data(value), success(true) {}

        explicit NodeEvaluationResult(const Vec3& value)
            : data(value), success(true) {}

        // Error result
        static NodeEvaluationResult makeError(const std::string& errorMsg) {
            NodeEvaluationResult result;
            result.success = false;
            result.error = errorMsg;
            return result;
        }

        // Check if result holds a specific type
        template<typename T>
        bool holds() const {
            return std::holds_alternative<T>(data);
        }

        // Get result value (returns nullptr if wrong type)
        template<typename T>
        const T* getPtr() const {
            return std::get_if<T>(&data);
        }
    };

    /**
     * @brief Context provided during node graph evaluation
     *
     * Contains:
     * - Current time/frame for animation
     * - Access to the owning node graph
     * - Result cache to avoid re-evaluating clean nodes
     */
    struct EvaluationContext
    {
        double currentTime = 0.0;        // Current time in seconds
        size_t currentFrame = 0;         // Current frame number
        const NodeGraph* graph = nullptr; // Access to the owning graph

        // Cache of evaluation results (node UID -> result)
        // Clean nodes can reuse cached results instead of re-evaluating
        std::unordered_map<size_t, NodeEvaluationResult> cache;

        EvaluationContext() = default;

        EvaluationContext(double time, size_t frame, const NodeGraph* nodeGraph)
            : currentTime(time), currentFrame(frame), graph(nodeGraph) {}

        // Check if a node's result is cached
        bool hasCache(size_t nodeUid) const {
            return cache.find(nodeUid) != cache.end();
        }

        // Get cached result for a node
        const NodeEvaluationResult* getCached(size_t nodeUid) const {
            auto it = cache.find(nodeUid);
            return (it != cache.end()) ? &it->second : nullptr;
        }

        // Store result in cache
        void cacheResult(size_t nodeUid, const NodeEvaluationResult& result) {
            cache[nodeUid] = result;
        }

        // Clear cache (call when scene/graph changes)
        void clearCache() {
            cache.clear();
        }
    };

} // namespace tracey
