#pragma once

#include "parameter.hpp"
#include "sop_node.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tracey
{
    namespace sops
    {
        // Catalog entry the frontend palette/inspector consume. Mirrors the
        // shape produced by `list_sop_node_catalog`.
        struct PortSpec
        {
            std::string name;
        };

        struct ParamSpec
        {
            std::string name;
            ParamType type;
            // Stringified default (so this struct stays variant-free).
            std::string defaultRepr;
        };

        struct CatalogEntry
        {
            std::string kind;
            std::string label;
            std::string category; // "Generators" | "Modifiers" | "Combiners" | "Output"
            std::vector<PortSpec> inputs;
            std::vector<PortSpec> outputs;
            std::vector<ParamSpec> params;
        };

        // Process-wide registry of SOP node types. Populated by
        // tracey::sops::registerBuiltinSops() at editor startup.
        class SopRegistry
        {
        public:
            using Factory = std::function<std::unique_ptr<SopNode>(size_t uid)>;

            static SopRegistry &instance();

            // Register a node type. `factory(uid)` constructs a fresh
            // instance; `entry` describes the node for the catalog.
            void registerType(CatalogEntry entry, Factory factory);

            // Construct a fresh node of the given kind, or nullptr if unknown.
            std::unique_ptr<SopNode> create(std::string_view kind, size_t uid) const;

            // All registered entries, in registration order.
            const std::vector<CatalogEntry> &catalog() const { return m_catalog; }

            bool has(std::string_view kind) const;

        private:
            SopRegistry() = default;
            std::vector<CatalogEntry> m_catalog;
            std::vector<Factory> m_factories;
        };

        // Populate the registry with the v1 built-in SOP set. Must be called
        // exactly once at startup before any cook / catalog query.
        //
        // Why explicit (rather than file-scope static-init): the SOP nodes
        // live in a static lib and would otherwise be dropped by the linker
        // because no symbol from each TU is externally referenced.
        // registerBuiltinSops() touches a function from each node TU,
        // forcing the link.
        void registerBuiltinSops();
    }
}
