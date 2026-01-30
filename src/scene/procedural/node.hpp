#pragma once

#include "../../graph/node.hpp"
#include "../../graph/port_info.hpp"
#include "parameter.hpp"
#include "evaluation_context.hpp"
#include <memory>
#include <unordered_map>
#include <string>

namespace tracey
{
    /**
     * @brief Node type enumeration for procedural nodes
     */
    enum class NodeType
    {
        Actor,              // Container with geometry network
        GeometryPrimitive,  // Primitive geometry (cube, sphere, etc.)
        GeometryTransform,  // Transform geometry
        GeometryMerge,      // Combine multiple geometries
        GeometryCopyToPoints, // Instance geometry to points
        Material,           // Material assignment
        Transform,          // Spatial transformation
        Camera,             // Camera definition
        MathFloat,          // Float math operations
        MathVector,         // Vector math operations
        Animation           // Animation/constraint nodes
    };

    /**
     * @brief Base class for all procedural nodes
     *
     * Extends the graph::Node base class with:
     * - Parameter management
     * - Evaluation capability
     * - Dirty propagation for incremental updates
     * - Input/output ports
     */
    class ProceduralNode : public Node
    {
    public:
        ProceduralNode(size_t uid, NodeType type, std::string name);
        virtual ~ProceduralNode() = default;

        // Basic accessors
        NodeType nodeType() const { return m_nodeType; }
        const std::string& name() const { return m_name; }
        void setName(const std::string& name) { m_name = name; }

        // Parameter management
        void addParameter(std::unique_ptr<Parameter> param);
        Parameter* getParameter(const std::string& name);
        const Parameter* getParameter(const std::string& name) const;
        const std::unordered_map<std::string, std::unique_ptr<Parameter>>& parameters() const {
            return m_parameters;
        }

        // Evaluation (pure virtual - must be implemented by derived classes)
        virtual NodeEvaluationResult evaluate(const EvaluationContext& ctx) = 0;

        // Dirty propagation for incremental updates
        void setDirty(bool dirty = true) { m_dirty = dirty; }
        bool isDirty() const { return m_dirty; }

        // Port management (for Phase 2 - node connections)
        // Currently stubbed for Phase 1
        virtual const InputsAndOutputs* ports() const { return nullptr; }

    protected:
        // Helper for derived classes to create parameters
        void addFloatParameter(const std::string& name, float defaultValue,
                              float minVal = 0.0f, float maxVal = 1.0f)
        {
            auto param = std::make_unique<Parameter>(name, ParameterType::Float, defaultValue);
            ParameterMetadata meta;
            meta.label = name;
            meta.minValue = minVal;
            meta.maxValue = maxVal;
            param->setMetadata(meta);
            addParameter(std::move(param));
        }

        void addVec3Parameter(const std::string& name, const Vec3& defaultValue)
        {
            auto param = std::make_unique<Parameter>(name, ParameterType::Vec3, defaultValue);
            ParameterMetadata meta;
            meta.label = name;
            param->setMetadata(meta);
            addParameter(std::move(param));
        }

        void addColorParameter(const std::string& name, const Vec3& defaultValue)
        {
            auto param = std::make_unique<Parameter>(name, ParameterType::Color, defaultValue);
            ParameterMetadata meta;
            meta.label = name;
            meta.uiWidget = "color_picker";
            param->setMetadata(meta);
            addParameter(std::move(param));
        }

    private:
        NodeType m_nodeType;
        std::string m_name;
        std::unordered_map<std::string, std::unique_ptr<Parameter>> m_parameters;
        bool m_dirty = true;  // Start dirty - needs initial evaluation
    };

} // namespace tracey
