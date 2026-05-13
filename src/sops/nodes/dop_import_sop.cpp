// dop_import — pull the current frame's simulation state out of the
// top-level DopGraph and make it available to the SOP graph for rendering.
//
// The SOP itself is intentionally simple: it owns a stamped Geometry,
// and its cook() returns a copy of it. EditorServer is responsible for
// keeping that buffer in sync with the DopGraph each time the playhead
// moves — it cooks the DOP forward, then walks the SOP graph and stamps
// the current frame's geometry into every dop_import via the free
// function setDopImportGeometry().
//
// This decoupling means dop_import doesn't take a dependency on
// src/dops/. The SOP cooks on the worker thread (async via the cook
// queue); the DOP graph is only touched on the main thread. Stamping
// happens on the main thread BEFORE the cook is posted, so the worker
// reads a stable, immutable copy.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "dop_import_sop.hpp"

#include "../../geometry/geometry.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class DopImportSop : public SopNode
            {
            public:
                explicit DopImportSop(size_t uid) : SopNode(uid) {}

                std::string kind() const override { return "dop_import"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    // No inputs — the data comes from the stamped buffer.
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    // Return a copy of the stamped geometry. SopGraph's cook
                    // cache hashes the result so unchanged frames don't
                    // trigger a downstream rebuild.
                    return m_stamped;
                }

                // The cook cache builds each node's key from params + inputs
                // + serializeExtraJson(). dop_import has no params and no
                // inputs, so without an `extra` field the key never changes
                // and the cache returns frame 1's stamped geometry forever —
                // the user sees one frozen particle. Phase C of the GPU-
                // resident refactor: read the P attribute's generation +
                // point count as an O(1) change signal. The DOP graph stamps
                // a fresh AttributeTable into m_stamped each frame and
                // Attribute<T>::clone() preserves the source generation, so
                // a different generation here means "different sim state"
                // without needing to hash N*sizeof(Vec3) bytes per cook.
                std::string serializeExtraJson() const override
                {
                    const auto *P = m_stamped.points().get<Vec3>("P");
                    const uint64_t gen = P ? P->generation() : 0;
                    const uint64_t n   = m_stamped.pointCount();
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                        "{\"stamp_gen\":\"%llu\",\"n\":\"%llu\"}",
                        static_cast<unsigned long long>(gen),
                        static_cast<unsigned long long>(n));
                    return std::string(buf);
                }
                // Intentionally NO matching deserializeExtraJson: the stamp
                // hash is a derived signal of m_stamped, not load-bearing
                // node state. m_stamped is always set by EditorServer's
                // CookRequest::dop_stamps side channel before the cook —
                // a loaded scene starts with an empty stamp and re-stamps
                // on the next playhead tick.

                Geometry m_stamped;
            };
        }

        void setDopImportGeometry(SopNode *node, Geometry geo)
        {
            auto *self = dynamic_cast<DopImportSop *>(node);
            if (!self) return;
            self->m_stamped = std::move(geo);
        }

        const Geometry *dopImportGeometry(const SopNode *node)
        {
            const auto *self = dynamic_cast<const DopImportSop *>(node);
            if (!self) return nullptr;
            return &self->m_stamped;
        }

        void registerDopImportSop()
        {
            SopRegistry::instance().registerType(
                {"dop_import", "DOP Import", "Generators",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<DopImportSop>(uid);
                });
        }
    }
}
