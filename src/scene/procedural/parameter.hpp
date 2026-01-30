#pragma once

#include "parameter_value.hpp"
#include <string>
#include <optional>
#include <vector>
#include <utility>

namespace tracey
{
    // Forward declaration for keyframe (will be implemented in Phase 4)
    struct Keyframe;

    /**
     * @brief Parameter type enumeration
     */
    enum class ParameterType
    {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Int,
        Bool,
        String,
        Color,      // RGB color (special vec3 with UI hint)
        Texture     // Texture reference (string path)
    };

    /**
     * @brief Parameter metadata for UI and validation
     */
    struct ParameterMetadata
    {
        std::string label;              // UI display name
        std::string tooltip;            // Help text for users
        float minValue = -FLT_MAX;      // Range constraints for numeric types
        float maxValue = FLT_MAX;
        std::vector<std::string> enumValues; // For enum-like parameters
        std::string uiWidget;           // UI hint: "slider", "color_picker", etc.

        ParameterMetadata() = default;
    };

    /**
     * @brief Flags controlling parameter behavior
     */
    enum class ParameterFlags : uint32_t
    {
        None = 0,
        Animatable = 1 << 0,        // Can be keyframed (Phase 4)
        Connectable = 1 << 1,       // Can accept connections from other nodes (Phase 3)
        Exposable = 1 << 2,         // Can be promoted to parent network
        Hidden = 1 << 3,            // Hidden from UI
        ReadOnly = 1 << 4           // Cannot be edited by user
    };

    inline ParameterFlags operator|(ParameterFlags a, ParameterFlags b) {
        return static_cast<ParameterFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline uint32_t operator&(ParameterFlags a, ParameterFlags b) {
        return static_cast<uint32_t>(a) & static_cast<uint32_t>(b);
    }

    /**
     * @brief Universal parameter class for all nodes
     *
     * Phase 1: Static values only
     * Phase 3: Connections and expressions
     * Phase 4: Keyframes and animation
     */
    class Parameter
    {
    public:
        Parameter(std::string name, ParameterType type, ParameterValue defaultValue);

        // Basic accessors
        const std::string& name() const { return m_name; }
        ParameterType type() const { return m_type; }

        // Value access
        const ParameterValue& value() const { return m_value; }
        void setValue(const ParameterValue& value);

        // Metadata
        const ParameterMetadata& metadata() const { return m_metadata; }
        void setMetadata(const ParameterMetadata& meta) { m_metadata = meta; }

        // Flags
        uint32_t flags() const { return m_flags; }
        void setFlags(uint32_t flags) { m_flags = flags; }
        bool isAnimatable() const { return m_flags & static_cast<uint32_t>(ParameterFlags::Animatable); }
        bool isConnectable() const { return m_flags & static_cast<uint32_t>(ParameterFlags::Connectable); }
        bool isExposable() const { return m_flags & static_cast<uint32_t>(ParameterFlags::Exposable); }

        // Connection management (Phase 3)
        // Currently stubbed for Phase 1
        bool hasConnection() const { return m_connectedOutput.has_value(); }
        void setConnection(size_t nodeUid, const std::string& portName);
        void clearConnection();
        const std::optional<std::pair<size_t, std::string>>& connection() const {
            return m_connectedOutput;
        }

        // Expression support (Phase 3)
        // Currently stubbed for Phase 1
        bool hasExpression() const { return !m_expression.empty(); }
        void setExpression(const std::string& expr) { m_expression = expr; }
        const std::string& expression() const { return m_expression; }

        // Keyframe animation (Phase 4)
        // Currently stubbed for Phase 1
        bool hasKeyframes() const;
        void addKeyframe(size_t frame, const ParameterValue& value);
        void removeKeyframe(size_t frame);
        ParameterValue evaluateAtTime(double time) const;

    private:
        std::string m_name;
        ParameterType m_type;
        ParameterValue m_value;
        ParameterMetadata m_metadata;
        uint32_t m_flags = 0;

        // Phase 3: Connection to another node's output
        std::optional<std::pair<size_t, std::string>> m_connectedOutput;

        // Phase 3: Expression string (e.g., "$F * 0.1")
        std::string m_expression;

        // Phase 4: Keyframe data (will be implemented later)
    };

} // namespace tracey
