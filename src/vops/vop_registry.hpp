#pragma once

#include "parameter.hpp"
#include "vop_node.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tracey
{
    namespace vops
    {
        // Mirrors src/sops/sop_registry.hpp's CatalogEntry / PortSpec / ParamSpec
        // shape so the editor's catalog wire format is identical between SOP
        // and VOP graphs (only the kind set differs).
        struct PortSpec
        {
            std::string name;
        };

        struct ParamSpec
        {
            std::string name;
            ParamType type;
            std::string defaultRepr; // stringified default
            // Optional UI hints — see src/sops/sop_registry.hpp ParamSpec
            // for full semantics. Default to "no hint" so existing
            // registrations keep working unchanged.
            double rangeMin = 0.0;
            double rangeMax = 0.0;
            double rangeStep = 0.0;
            std::vector<std::string> options{};
        };

        struct CatalogEntry
        {
            std::string kind;
            std::string label;
            // Categories used by the v1 palette: "Bind" (read/write attribute
            // hooks), "Constants", "Math", "Vector", "Noise". Free-form
            // strings on the wire — UI groups by string equality.
            std::string category;
            std::vector<PortSpec> inputs;
            std::vector<PortSpec> outputs;
            std::vector<ParamSpec> params;
        };

        // Process-wide registry of VOP node types. Populated by
        // tracey::vops::registerBuiltinVops() at editor startup.
        class VopRegistry
        {
        public:
            using Factory = std::function<std::unique_ptr<VopNode>(size_t uid)>;

            static VopRegistry &instance();

            void registerType(CatalogEntry entry, Factory factory);
            std::unique_ptr<VopNode> create(std::string_view kind, size_t uid) const;
            const std::vector<CatalogEntry> &catalog() const { return m_catalog; }
            bool has(std::string_view kind) const;

        private:
            VopRegistry() = default;
            std::vector<CatalogEntry> m_catalog;
            std::vector<Factory> m_factories;
        };
    }
}
