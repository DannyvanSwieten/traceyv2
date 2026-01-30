#include "parameter.hpp"
#include <stdexcept>

namespace tracey
{
    Parameter::Parameter(std::string name, ParameterType type, ParameterValue defaultValue)
        : m_name(std::move(name))
        , m_type(type)
        , m_value(std::move(defaultValue))
        , m_flags(0)
    {
    }

    void Parameter::setValue(const ParameterValue& value)
    {
        // TODO: Type validation - ensure value type matches parameter type
        m_value = value;
    }

    void Parameter::setConnection(size_t nodeUid, const std::string& portName)
    {
        // Phase 3 implementation
        m_connectedOutput = std::make_pair(nodeUid, portName);
    }

    void Parameter::clearConnection()
    {
        // Phase 3 implementation
        m_connectedOutput = std::nullopt;
    }

    bool Parameter::hasKeyframes() const
    {
        // Phase 4 implementation
        return false;  // No keyframes in Phase 1
    }

    void Parameter::addKeyframe(size_t frame, const ParameterValue& value)
    {
        // Phase 4 implementation
        // For now, just store the keyframe
        // TODO: Sort by frame, handle duplicate frames
        (void)frame;
        (void)value;
        // m_keyframes will be properly implemented in Phase 4
    }

    void Parameter::removeKeyframe(size_t frame)
    {
        // Phase 4 implementation
        (void)frame;
        // TODO: Find and remove keyframe at specified frame
    }

    ParameterValue Parameter::evaluateAtTime(double time) const
    {
        // Phase 4 implementation
        // For now, just return static value
        // TODO: Implement keyframe interpolation
        (void)time;

        // Priority order (will be implemented in later phases):
        // 1. Connection - if connected to another node's output
        // 2. Expression - if expression is set
        // 3. Keyframe - if keyframes exist, interpolate at current time
        // 4. Static value - return stored value

        return m_value;
    }

} // namespace tracey
