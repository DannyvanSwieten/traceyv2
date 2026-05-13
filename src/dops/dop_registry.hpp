#pragma once

#include "dop_node.hpp"
#include "parameter.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tracey
{
    namespace dops
    {
        // Mirrors src/vops/vop_registry.hpp's CatalogEntry shape so the
        // editor's wire format is identical across the three graph kinds —
        // only the registered kind set differs.
        struct PortSpec
        {
            std::string name;
        };

        struct ParamSpec
        {
            std::string name;
            ParamType type;
            std::string defaultRepr;
            double rangeMin = 0.0;
            double rangeMax = 0.0;
            double rangeStep = 0.0;
            std::vector<std::string> options;
        };

        struct CatalogEntry
        {
            std::string kind;
            std::string label;
            // Phase 1 categories: "Source" (particle emission), "Force"
            // (force-accumulator nodes), "Solver" (integrators), "Modify"
            // (kill, scatter, etc.). Free-form on the wire — the UI groups
            // by string equality.
            std::string category;
            std::vector<PortSpec> inputs;
            std::vector<PortSpec> outputs;
            std::vector<ParamSpec> params;
        };

        // Process-wide registry of DOP node types. Populated by
        // tracey::dops::registerBuiltinDops() at editor startup.
        class DopRegistry
        {
        public:
            using Factory = std::function<std::unique_ptr<DopNode>(size_t uid)>;

            static DopRegistry &instance();

            void registerType(CatalogEntry entry, Factory factory);
            std::unique_ptr<DopNode> create(std::string_view kind, size_t uid) const;
            const std::vector<CatalogEntry> &catalog() const { return m_catalog; }
            bool has(std::string_view kind) const;

        private:
            DopRegistry() = default;
            std::vector<CatalogEntry> m_catalog;
            std::vector<Factory> m_factories;
        };
    }
}
