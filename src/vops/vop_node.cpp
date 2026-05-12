#include "vop_node.hpp"

// Mirrors src/sops/sop_node.cpp — VOP and SOP nodes share the parameter
// machinery but live in separate type hierarchies. v1 omits the *At
// time-sampled lookups since VOP param animation is deferred (decision H
// in the plan).

namespace tracey
{
    namespace vops
    {
        namespace
        {
            Parameter *findParam(std::vector<Parameter> &params, std::string_view name)
            {
                for (auto &p : params) if (p.name == name) return &p;
                return nullptr;
            }
            const Parameter *findParam(const std::vector<Parameter> &params, std::string_view name)
            {
                for (const auto &p : params) if (p.name == name) return &p;
                return nullptr;
            }
        }

        void VopNode::declareParam(Parameter p)
        {
            if (auto *existing = findParam(m_params, p.name))
            {
                *existing = std::move(p);
                return;
            }
            m_params.push_back(std::move(p));
        }

        float VopNode::paramFloat(std::string_view name, float def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Float) return def;
            if (auto *v = std::get_if<float>(&p->value)) return *v;
            return def;
        }
        int VopNode::paramInt(std::string_view name, int def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Int) return def;
            if (auto *v = std::get_if<int>(&p->value)) return *v;
            return def;
        }
        bool VopNode::paramBool(std::string_view name, bool def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Bool) return def;
            if (auto *v = std::get_if<bool>(&p->value)) return *v;
            return def;
        }
        Vec3 VopNode::paramVec3(std::string_view name, Vec3 def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Vec3) return def;
            if (auto *v = std::get_if<Vec3>(&p->value)) return *v;
            return def;
        }
        std::string VopNode::paramString(std::string_view name, std::string def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::String) return def;
            if (auto *v = std::get_if<std::string>(&p->value)) return *v;
            return def;
        }

        void VopNode::setParamFloat(std::string_view name, float v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Float; p->value = v; }
        }
        void VopNode::setParamInt(std::string_view name, int v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Int; p->value = v; }
        }
        void VopNode::setParamBool(std::string_view name, bool v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Bool; p->value = v; }
        }
        void VopNode::setParamVec3(std::string_view name, Vec3 v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Vec3; p->value = v; }
        }
        void VopNode::setParamString(std::string_view name, std::string v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::String; p->value = std::move(v); }
        }
    }
}
