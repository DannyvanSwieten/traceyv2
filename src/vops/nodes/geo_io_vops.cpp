// Unified geometry input + output nodes — Houdini-style "Geometry VOP"
// network terminals. Each VOP graph used to need a clutter of per-
// attribute bind_in_X / bind_out_X nodes (P, N, Cd, uv, ...); these
// two new node kinds collapse the whole zoo into a single input + a
// single output, with named ports for every standard attribute.
//
// The old `bind_*` nodes are kept in the registry for backwards
// compatibility with existing scenes, but new graphs should reach for
// `geo_input` / `geo_output` instead.
//
// Unconnected-input policy on `geo_output` follows Houdini: each input
// port has a boolean `passthrough_<port>` param. When the port is left
// unconnected:
//   • passthrough = true  → the attribute on the geometry is NOT
//                           touched (matches the old preset
//                           bind_out_X with createIfMissing=false).
//   • passthrough = false → the kernel writes the canonical default
//                           for that attribute (white for Cd, up for
//                           N, identity for pscale / Alpha, zero for
//                           the rest). Useful for graphs that want
//                           to clear an attribute on every cook.
//
// The bool defaults to true — the safest "do nothing surprising"
// setting that lets a freshly-dropped geo_output node be a no-op until
// the user wires it up.

#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"
#include "../geo_io_ports.hpp"

#include "../../core/types.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"
#include "../../geometry/geometry.hpp"

#include <array>
#include <string>

namespace tracey
{
    namespace vops
    {
        namespace
        {
            // Port specs live in geo_io_ports.hpp — shared with the GPU
            // emitter and the dispatcher so the port-index contract and
            // the canonical defaults can't drift between the three.
            constexpr const auto &kVecPorts           = kGeoVecPorts;
            constexpr const auto &kFloatPorts         = kGeoFloatPorts;
            constexpr const auto &kInputOnlyFloatPorts = kGeoReadOnlyFloatPorts;
        }

        // ── geo_input ────────────────────────────────────────────────────────
        // One output port per standard attribute. Reads each attribute from
        // the geometry's point table; falls back to the canonical default
        // when the attribute is missing — same fallback the dispatcher uses
        // when it materialises an unbacked attribute.
        class GeoInputVop : public VopNode
        {
        public:
            explicit GeoInputVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "geo_input"; }

            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                // Order: vec3 ports first, then floats, then read-only floats.
                for (const auto &p : kVecPorts)
                    io.addOutput(PortInfo::createOutput(p.name, DataType::Vec3));
                for (const auto &p : kFloatPorts)
                    io.addOutput(PortInfo::createOutput(p.name, DataType::Float));
                for (const auto &p : kInputOnlyFloatPorts)
                    io.addOutput(PortInfo::createOutput(p.name, DataType::Float));
                return io;
            }

            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;

                size_t portIdx = 0;
                for (const auto &p : kVecPorts)
                {
                    Vec3 v = p.defaultValue;
                    if (const auto *a = ctx.geometry->points().get<Vec3>(p.name))
                    {
                        if (ctx.pointIndex < a->data().size()) v = a->data()[ctx.pointIndex];
                    }
                    ctx.graph->writeOutput(ctx, uid(), portIdx++, v);
                }
                for (const auto &p : kFloatPorts)
                {
                    float v = p.defaultValue;
                    if (const auto *a = ctx.geometry->points().get<float>(p.name))
                    {
                        if (ctx.pointIndex < a->data().size()) v = a->data()[ctx.pointIndex];
                    }
                    ctx.graph->writeOutput(ctx, uid(), portIdx++, v);
                }
                // age / life: same shape, but with their own defaults.
                for (size_t i = 0; i < 2; ++i)
                {
                    const auto &p = kInputOnlyFloatPorts[i];
                    float v = p.defaultValue;
                    if (const auto *a = ctx.geometry->points().get<float>(p.name))
                    {
                        if (ctx.pointIndex < a->data().size()) v = a->data()[ctx.pointIndex];
                    }
                    ctx.graph->writeOutput(ctx, uid(), portIdx++, v);
                }
                // ptnum comes from the current point index, not an attribute.
                ctx.graph->writeOutput(ctx, uid(), portIdx,
                                       static_cast<float>(ctx.pointIndex));
            }
        };

        // ── geo_output ───────────────────────────────────────────────────────
        // One input port per writable standard attribute. Each port has a
        // `passthrough_<name>` bool param controlling the unconnected
        // behaviour. When connected, the upstream value is written to the
        // attribute at every point (materialising the attribute on the
        // geometry if needed). When unconnected:
        //   • passthrough_<name> = true  → leave the attribute alone.
        //   • passthrough_<name> = false → stamp the canonical default
        //                                  at every point (and materialise
        //                                  the attribute if missing).
        class GeoOutputVop : public VopNode
        {
        public:
            explicit GeoOutputVop(size_t uid) : VopNode(uid)
            {
                // Per-port passthrough toggle. true by default so a fresh
                // geo_output node with no wires is a no-op rather than
                // surprise-stamping every attribute on every point.
                for (const auto &p : kVecPorts)
                    declareParam(Parameter::makeBool(
                        std::string("passthrough_") + p.name, true));
                for (const auto &p : kFloatPorts)
                    declareParam(Parameter::makeBool(
                        std::string("passthrough_") + p.name, true));
            }
            std::string kind() const override { return "geo_output"; }

            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                for (const auto &p : kVecPorts)
                    io.addInput(PortInfo::createInput(p.name, DataType::Vec3));
                for (const auto &p : kFloatPorts)
                    io.addInput(PortInfo::createInput(p.name, DataType::Float));
                return io;
            }

            // prepare() runs once per cook, on the worker thread, before
            // the per-point evaluate loop. Materialise any attribute that
            // will receive a write so evaluate() doesn't have to add it
            // on the hot path (and so the parallel-for doesn't race two
            // threads adding the same attribute).
            void prepare(Geometry &geo) const override
            {
                size_t inputIdx = 0;
                for (const auto &p : kVecPorts)
                {
                    const bool willWrite = needsWrite(inputIdx);
                    ++inputIdx;
                    if (!willWrite) continue;
                    if (!geo.points().get<Vec3>(p.name))
                        geo.points().add<Vec3>(p.name, p.defaultValue);
                }
                for (const auto &p : kFloatPorts)
                {
                    const bool willWrite = needsWrite(inputIdx);
                    ++inputIdx;
                    if (!willWrite) continue;
                    if (!geo.points().get<float>(p.name))
                        geo.points().add<float>(p.name, p.defaultValue);
                }
            }

            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;

                size_t inputIdx = 0;
                // Vec3 ports
                for (const auto &p : kVecPorts)
                {
                    const bool passthrough = paramBool(
                        std::string("passthrough_") + p.name, true);
                    auto in = ctx.graph->readInput(ctx, uid(), inputIdx);
                    ++inputIdx;

                    Vec3 v;
                    if (in)
                    {
                        if (auto *vv = std::get_if<Vec3>(&*in)) v = *vv;
                        else if (auto *f = std::get_if<float>(&*in)) v = Vec3(*f);
                        else if (auto *i = std::get_if<int>(&*in)) v = Vec3(static_cast<float>(*i));
                        else v = p.defaultValue;
                    }
                    else
                    {
                        // No upstream connection. passthrough=true is a
                        // no-op; passthrough=false stamps the default.
                        if (passthrough) continue;
                        v = p.defaultValue;
                    }

                    if (auto *a = ctx.geometry->points().get<Vec3>(p.name))
                    {
                        if (ctx.pointIndex < a->data().size())
                            a->data()[ctx.pointIndex] = v;
                    }
                }
                // Float ports
                for (const auto &p : kFloatPorts)
                {
                    const bool passthrough = paramBool(
                        std::string("passthrough_") + p.name, true);
                    auto in = ctx.graph->readInput(ctx, uid(), inputIdx);
                    ++inputIdx;

                    float v;
                    if (in)
                    {
                        if (auto *f = std::get_if<float>(&*in)) v = *f;
                        else if (auto *vv = std::get_if<Vec3>(&*in)) v = vv->x;
                        else if (auto *i = std::get_if<int>(&*in)) v = static_cast<float>(*i);
                        else v = p.defaultValue;
                    }
                    else
                    {
                        if (passthrough) continue;
                        v = p.defaultValue;
                    }

                    if (auto *a = ctx.geometry->points().get<float>(p.name))
                    {
                        if (ctx.pointIndex < a->data().size())
                            a->data()[ctx.pointIndex] = v;
                    }
                }
            }

            // Does this input port need to be written to the geometry on
            // this cook? Connected → yes. Unconnected + passthrough=false
            // → yes (stamp default). Unconnected + passthrough=true → no.
            // Exposed for the GPU emitter so it can decide which SSBOs
            // to bind for writes.
            bool needsWrite(size_t portIdx) const
            {
                const char *name = portNameAt(portIdx);
                if (!name) return false;
                const bool passthrough = paramBool(
                    std::string("passthrough_") + name, true);
                // If the graph has a connection feeding this port, the
                // graph-side check at the caller has already returned
                // true; this helper covers the unconnected branch. The
                // emitter / evaluator OR them together: needsWrite ||
                // hasConnection. Either condition triggers the
                // materialise + per-point write.
                return !passthrough;
            }

            // Port-index → attribute name. Indices match `ports()` order:
            // [0..kVecPorts.size()) are Vec3, then float ports.
            const char *portNameAt(size_t portIdx) const
            {
                if (portIdx < kVecPorts.size())
                    return kVecPorts[portIdx].name;
                portIdx -= kVecPorts.size();
                if (portIdx < kFloatPorts.size())
                    return kFloatPorts[portIdx].name;
                return nullptr;
            }
        };

        namespace
        {
            template <typename T>
            VopRegistry::Factory makeFactory()
            {
                return [](size_t uid) { return std::make_unique<T>(uid); };
            }
        }

        void registerGeoIoVops()
        {
            auto &reg = VopRegistry::instance();

            // Build the port-spec arrays expected by the CatalogEntry from
            // our kVec/kFloat tables so the registry rows can't drift out
            // of sync with the node implementation.
            std::vector<PortSpec> inputOutputs;
            for (const auto &p : kVecPorts)             inputOutputs.push_back({p.name});
            for (const auto &p : kFloatPorts)           inputOutputs.push_back({p.name});

            std::vector<PortSpec> readOnlyExtras = inputOutputs;
            for (const auto &p : kInputOnlyFloatPorts)  readOnlyExtras.push_back({p.name});

            reg.registerType(
                {"geo_input", "Geometry Input", "Geometry I/O",
                 /*inputs*/  {},
                 /*outputs*/ readOnlyExtras,
                 /*params*/  {}},
                makeFactory<GeoInputVop>());

            // Build geo_output's params list: one bool per writable port.
            std::vector<ParamSpec> outputParams;
            outputParams.reserve(kVecPorts.size() + kFloatPorts.size());
            auto addBoolParam = [&](const char *attrName) {
                ParamSpec ps;
                ps.name = std::string("passthrough_") + attrName;
                ps.type = ParamType::Bool;
                ps.defaultRepr = "true";
                outputParams.push_back(std::move(ps));
            };
            for (const auto &p : kVecPorts)   addBoolParam(p.name);
            for (const auto &p : kFloatPorts) addBoolParam(p.name);

            reg.registerType(
                {"geo_output", "Geometry Output", "Geometry I/O",
                 /*inputs*/  inputOutputs,
                 /*outputs*/ {},
                 /*params*/  std::move(outputParams)},
                makeFactory<GeoOutputVop>());
        }
    }
}
