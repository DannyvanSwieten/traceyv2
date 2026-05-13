#pragma once

#include "../graph/node.hpp"
#include "../graph/port_info.hpp"
#include "eval_context.hpp"
#include "parameter.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tracey
{
    namespace dops
    {
        // Abstract base for all DOP nodes.
        //
        // A DopGraph cooks one frame at a time, walking its nodes in topo
        // order. Each node's cookFrame(ctx) mutates `ctx.state` in place —
        // pop_source appends particles, pop_gravity accumulates into the
        // `force` point attribute, pop_solver integrates v and P. Inter-node
        // data wires are deferred: Phase 0 / Phase 1 keep the graph as a
        // linear pipeline (sources → forces → solver), with no slot table.
        //
        // Invariants mirror VopNode:
        //   • cookFrame() must be const (graph cook is const).
        //   • kind() is the stable id, registered in DopRegistry.
        //   • Parameters are constants; no port wiring.
        //   • prepare(SimState&) runs once per frame before the substep loop;
        //     use it to allocate point attributes the node needs (e.g.
        //     pop_solver ensures `v` and `force` exist).
        class DopNode : public Node
        {
        public:
            explicit DopNode(size_t uid) : Node(uid) {}
            ~DopNode() override = default;

            virtual std::string kind() const = 0;
            virtual InputsAndOutputs ports() const = 0;

            // Per-substep work. Reads / mutates ctx.state (which owns the
            // Geometry being advanced) and reads ctx.state->header for
            // (frame, time, dt, substepIdx).
            virtual void cookFrame(DopEvalContext &ctx) const = 0;

            // Optional one-shot setup called once per frame before the
            // substep loop. Use to ensure point attributes exist (so
            // cookFrame doesn't pay the cost per substep).
            virtual void prepare(SimState & /*state*/) const {}

            // Generic per-node JSON extension hook. DOP nodes that own
            // non-DopGraph child state (pop_force hosting a VopGraph,
            // future nodes hosting collision meshes, etc.) override these
            // to round-trip that state. Mirrors SopNode::serializeExtraJson.
            // Empty string means "nothing to serialize" (default).
            virtual std::string serializeExtraJson() const { return {}; }
            virtual void deserializeExtraJson(const std::string & /*jsonText*/) {}

            // ── Parameters (mirror VopNode helpers) ──
            const std::vector<Parameter> &parameters() const { return m_params; }
            std::vector<Parameter> &parameters() { return m_params; }

            void declareParam(Parameter p);

            float       paramFloat (std::string_view name, float def = 0.0f) const;
            int         paramInt   (std::string_view name, int def = 0) const;
            bool        paramBool  (std::string_view name, bool def = false) const;
            Vec3        paramVec3  (std::string_view name, Vec3 def = Vec3(0.0f)) const;
            std::string paramString(std::string_view name, std::string def = {}) const;

            void setParamFloat (std::string_view name, float v);
            void setParamInt   (std::string_view name, int v);
            void setParamBool  (std::string_view name, bool v);
            void setParamVec3  (std::string_view name, Vec3 v);
            void setParamString(std::string_view name, std::string v);

            // ── Graph editor cosmetic state ──
            float posX() const { return m_posX; }
            float posY() const { return m_posY; }
            void setPos(float x, float y) { m_posX = x; m_posY = y; }

        private:
            std::vector<Parameter> m_params;
            float m_posX = 0.0f;
            float m_posY = 0.0f;
        };
    }
}
