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
    class Geometry;

    namespace vops
    {
        // Abstract base for all VOP nodes.
        //
        // VOP nodes operate on per-point attribute values. A VopGraph runs them
        // in topological order; each `evaluate(ctx)` call reads inputs from the
        // current EvalContext::slots, computes outputs, writes them back.
        //
        // v1 invariants (mirroring SopNode's invariants):
        //   • evaluate() is a pure function of inputs + own parameters.
        //   • kind() is a stable string id, registered in VopRegistry.
        //   • Parameters are constants (no port wiring) — keeps the model simple.
        //   • prepare(Geometry&) runs once per cook *before* the per-point loop;
        //     bind_out_attr_* uses it to add the target attribute if missing.
        class VopNode : public Node
        {
        public:
            explicit VopNode(size_t uid) : Node(uid) {}
            ~VopNode() override = default;

            virtual std::string kind() const = 0;
            virtual InputsAndOutputs ports() const = 0;

            // Per-point evaluation. Reads upstream values from ctx.slots,
            // writes its own outputs back. Must be const — VOP graphs cook
            // inside a const SopNode::cook().
            virtual void evaluate(EvalContext &ctx) const = 0;

            // Optional one-shot setup called once per cook before the per-point
            // loop. Use to allocate / ensure attributes exist on the geometry,
            // not for per-point work.
            virtual void prepare(Geometry & /*geo*/) const {}

            // ── Parameters (mirror SopNode helpers; see src/sops/sop_node.hpp) ──
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
