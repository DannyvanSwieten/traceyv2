#include "sop_node.hpp"

namespace tracey
{
    namespace sops
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

        void SopNode::declareParam(Parameter p)
        {
            if (auto *existing = findParam(m_params, p.name))
            {
                *existing = std::move(p);
                return;
            }
            m_params.push_back(std::move(p));
        }

        float SopNode::paramFloat(std::string_view name, float def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Float) return def;
            if (auto *v = std::get_if<float>(&p->value)) return *v;
            return def;
        }
        int SopNode::paramInt(std::string_view name, int def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Int) return def;
            if (auto *v = std::get_if<int>(&p->value)) return *v;
            return def;
        }
        bool SopNode::paramBool(std::string_view name, bool def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Bool) return def;
            if (auto *v = std::get_if<bool>(&p->value)) return *v;
            return def;
        }
        Vec3 SopNode::paramVec3(std::string_view name, Vec3 def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::Vec3) return def;
            if (auto *v = std::get_if<Vec3>(&p->value)) return *v;
            return def;
        }
        std::string SopNode::paramString(std::string_view name, std::string def) const
        {
            const auto *p = findParam(m_params, name);
            if (!p || p->type != ParamType::String) return def;
            if (auto *v = std::get_if<std::string>(&p->value)) return *v;
            return def;
        }

        void SopNode::setParamFloat(std::string_view name, float v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Float; p->value = v; }
        }
        void SopNode::setParamInt(std::string_view name, int v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Int; p->value = v; }
        }
        void SopNode::setParamBool(std::string_view name, bool v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Bool; p->value = v; }
        }
        void SopNode::setParamVec3(std::string_view name, Vec3 v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::Vec3; p->value = v; }
        }
        void SopNode::setParamString(std::string_view name, std::string v)
        {
            if (auto *p = findParam(m_params, name)) { p->type = ParamType::String; p->value = std::move(v); }
        }
    }
}
