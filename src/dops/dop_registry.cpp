#include "dop_registry.hpp"

namespace tracey
{
    namespace dops
    {
        DopRegistry &DopRegistry::instance()
        {
            static DopRegistry s_instance;
            return s_instance;
        }

        void DopRegistry::registerType(CatalogEntry entry, Factory factory)
        {
            m_catalog.push_back(std::move(entry));
            m_factories.push_back(std::move(factory));
        }

        std::unique_ptr<DopNode> DopRegistry::create(std::string_view kind, size_t uid) const
        {
            for (size_t i = 0; i < m_catalog.size(); ++i)
            {
                if (m_catalog[i].kind == kind) return m_factories[i](uid);
            }
            return nullptr;
        }

        bool DopRegistry::has(std::string_view kind) const
        {
            for (const auto &e : m_catalog) if (e.kind == kind) return true;
            return false;
        }
    }
}
