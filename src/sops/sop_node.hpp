#pragma once

#include "../geometry/geometry.hpp"
#include "../graph/node.hpp"
#include "../graph/port_info.hpp"
#include "parameter.hpp"

#include <span>
#include <string>
#include <vector>

namespace tracey
{
    class Transform;

    namespace sops
    {
        class SopGraph;

        // Output produced by a terminal SOP node (ObjectOutput) — collected at
        // graph cook time and turned into an Actor + SceneInstance + SceneObject
        // by the editor host.
        //
        // We hold a Transform pointer (forward-declared) so this header doesn't
        // pull in the full scene/transform.hpp. The implementation file uses
        // a fully-qualified Transform.
        struct EmittedActor
        {
            // uid of the ObjectOutput node that produced this actor. Lets the
            // editor host build a stable actor↔SOP mapping without relying
            // on iteration order.
            size_t sourceNodeUid = 0;

            // uid of the enclosing subnet node, or 0 when this actor is
            // emitted at the root of the SOP graph. Subnets push themselves
            // first into the emitted list (as a marker, see isSubnetMarker),
            // then recurse into their inner graph and stamp this field on
            // each child so apply_emitted can wire Actor::addChild edges.
            size_t parentNodeUid = 0;

            // True for the synthetic actor a subnet emits to represent itself
            // — a transform-only parent node with no geometry / instance /
            // material. apply_emitted skips object/instance/material setup
            // for these.
            bool isSubnetMarker = false;

            // True when this actor represents a /obj-style light (`light`
            // SOP terminal). apply_emitted creates a transform-only actor
            // and attaches the `light` payload below as a component, which
            // the SceneCompiler picks up into its light list. Lights and
            // subnet markers are mutually exclusive.
            bool isLight = false;
            // Light type as an integer matching tracey::LightType so this
            // header doesn't have to include scene/light.hpp. apply_emitted
            // translates back to the enum.
            int lightType = 0;
            Vec3 lightColor{1.0f, 1.0f, 1.0f};
            float lightIntensity = 1.0f;

            std::string name;
            Geometry geometry;
            // Transform is held as PODs so this struct stays trivially
            // copyable and codegen-friendly.
            Vec3 translate{0.0f};
            // Quaternion (wxyz) — identity by default.
            Vec4 rotation{1.0f, 0.0f, 0.0f, 0.0f};
            Vec3 scale{1.0f};
            std::string materialLibraryName;
        };

        // Abstract base for all SOP nodes.
        //
        // v1 invariants (see plan: "Designed for future graph→C++/dylib codegen"):
        //   • cook() is a pure function of inputs + own parameters.
        //   • kind() is a stable string id, registered in SopRegistry.
        //   • Parameters are a uniform typed table (no hardcoded member fields).
        //   • No std::function callbacks held inside the node; virtuals only.
        class SopNode : public Node
        {
        public:
            explicit SopNode(size_t uid) : Node(uid) {}
            ~SopNode() override = default;

            // Stable string identifier, e.g. "primitive_cube", "transform",
            // "object_output". Registered in SopRegistry. Used for both
            // serialization and codegen dispatch.
            virtual std::string kind() const = 0;

            // Port layout. Inputs feed `cook` positionally (input[0], input[1], ...).
            // Most generators have zero inputs and one output; modifiers have one
            // of each; merge has many inputs and one output; object_output has one
            // input and zero outputs (terminal).
            virtual InputsAndOutputs ports() const = 0;

            // Pure cook function. Returns the geometry produced by this node
            // given its (already-cooked) inputs. Will be called at most once
            // per (graph dirty epoch, node uid) pair — SopGraph caches results.
            //
            // Inputs may contain nullptr for optional/disconnected ports;
            // implementations must handle that by either ignoring the slot or
            // producing an empty/default Geometry.
            virtual Geometry cook(std::span<const Geometry *const> inputs) const = 0;

            // Time-aware variant. Default delegates to time-independent cook;
            // only nodes that animate something internally (attribute_vop with
            // promoted host params, for example) override this. SopGraph::cook
            // always calls cookAt so the time threads through the topo walk
            // and into recursive subnet sub-graphs uniformly.
            virtual Geometry cookAt(std::span<const Geometry *const> inputs,
                                    double /*time*/) const
            {
                return cook(inputs);
            }

            // ── Nested sub-graphs ──
            // A SOP node that wraps a sub-graph (subnet) returns a non-null
            // pointer to the inner graph here. The default returns nullptr —
            // overridden only by SubnetSop. Used by SopGraph::cook() to drive
            // the recursive emit pass and by editor_server's recursive
            // findNode helper for keyframe edits.
            virtual SopGraph *innerGraph() { return nullptr; }
            virtual const SopGraph *innerGraph() const { return nullptr; }
            // Attach an inner graph. Default no-op; only SubnetSop overrides.
            // Called by deserialization when a `subgraph` field is found on a
            // node, and lazily by editor_server when a freshly-created subnet
            // node has no inner graph yet.
            virtual void setInnerGraph(std::unique_ptr<SopGraph> /*g*/) {}

            // ── Generic per-node JSON extension hook ──
            // SOP nodes that own non-SopGraph child state (e.g. AttributeVopSop
            // hosting a VopGraph) override these to round-trip that state.
            // Returned strings are valid JSON snippets — the serializer parses
            // them back into the node JSON under an "extra" field. Empty
            // string means "nothing to serialize" (default).
            virtual std::string serializeExtraJson() const { return {}; }
            virtual void deserializeExtraJson(const std::string & /*jsonText*/) {}

            // ── Parameters ──
            const std::vector<Parameter> &parameters() const { return m_params; }
            std::vector<Parameter> &parameters() { return m_params; }

            // Add (or replace) a named parameter. Subclasses call this in their
            // constructor to declare their parameter set.
            void declareParam(Parameter p);

            // Lookup helpers — return defaults if the param is missing or has
            // the wrong type. cook() implementations should use these instead
            // of hand-walking m_params.
            float       paramFloat(std::string_view name, float def = 0.0f) const;
            int         paramInt(std::string_view name, int def = 0) const;
            bool        paramBool(std::string_view name, bool def = false) const;
            Vec3        paramVec3(std::string_view name, Vec3 def = Vec3(0.0f)) const;
            std::string paramString(std::string_view name, std::string def = {}) const;

            // Time-sampled lookups. For non-animated parameters they return
            // the same value as the constant accessors above. For animated
            // parameters they evaluate the keyframe channel(s) at `time`
            // (seconds). Vec3 is sampled per-component.
            float paramFloatAt(std::string_view name, double time, float def = 0.0f) const;
            int   paramIntAt  (std::string_view name, double time, int   def = 0) const;
            bool  paramBoolAt (std::string_view name, double time, bool  def = false) const;
            Vec3  paramVec3At (std::string_view name, double time, Vec3  def = Vec3(0.0f)) const;

            // Direct typed setters (used by deserialization + the editor host
            // when the JSON push arrives).
            void setParamFloat(std::string_view name, float v);
            void setParamInt(std::string_view name, int v);
            void setParamBool(std::string_view name, bool v);
            void setParamVec3(std::string_view name, Vec3 v);
            void setParamString(std::string_view name, std::string v);

            // ── Graph editor cosmetic state ──
            // 2D position the canvas places the node at. Echoed in serialization;
            // does not affect cook semantics.
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
