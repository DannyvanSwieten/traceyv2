#include "node.hpp"

namespace tracey
{
    ProceduralNode::ProceduralNode(size_t uid, NodeType type, std::string name)
        : Node(uid)
        , m_nodeType(type)
        , m_name(std::move(name))
        , m_dirty(true)
    {
    }

    void ProceduralNode::addParameter(std::unique_ptr<Parameter> param)
    {
        if (param) {
            m_parameters[param->name()] = std::move(param);
        }
    }

    Parameter* ProceduralNode::getParameter(const std::string& name)
    {
        auto it = m_parameters.find(name);
        return (it != m_parameters.end()) ? it->second.get() : nullptr;
    }

    const Parameter* ProceduralNode::getParameter(const std::string& name) const
    {
        auto it = m_parameters.find(name);
        return (it != m_parameters.end()) ? it->second.get() : nullptr;
    }

} // namespace tracey
