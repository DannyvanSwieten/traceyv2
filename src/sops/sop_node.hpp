#pragma once

#include "../geometry/geometry.hpp"
#include "../graph/node.hpp"
#include "../graph/port_info.hpp"
#include "parameter.hpp"

#include <memory>
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
            // Held by shared_ptr so the `instance` SOP can emit N actors
            // all referencing the same stamp without N deep-copies. The
            // 120-particle path-traced case used to memcpy the stamp's
            // vertex/attribute tables once per particle every cook —
            // hundreds of KB/cook for what is structurally one mesh.
            // Now it's one alloc + N shared_ptr copies. Lights and subnet
            // markers leave this null; consumers either gate on isLight /
            // isSubnetMarker before dereferencing, or use
            // `geometry_dedup_hash` which treats null as the empty digest.
            std::shared_ptr<const Geometry> geometry;
            // Transform is held as PODs so this struct stays trivially
            // copyable and codegen-friendly.
            Vec3 translate{0.0f};
            // Quaternion (wxyz) — identity by default.
            Vec4 rotation{1.0f, 0.0f, 0.0f, 0.0f};
            Vec3 scale{1.0f};
            std::string materialLibraryName;
            // Legacy field — kept for source compatibility with the rest
            // of the editor's emit pipeline (actor key composition, etc.),
            // but always 0 for the new instance-group emit path below.
            // The old "instance emits N EmittedActors" model lives only
            // as a fallback if some future caller wants it.
            uint32_t instanceIndex = 0;

            // Per-instance overrides for the `instance` SOP. When this
            // vector is non-empty, the actor represents an instance
            // group: ONE Scene Actor whose `instances()` list gets one
            // SceneInstance per entry here, each carrying the entry's
            // transform and per-instance tint. The `translate`/`rotation`
            // /`scale`/`tint` fields above are ignored in this case (the
            // group Actor sits at identity; each TLAS entry carries the
            // full per-instance transform via SceneInstance::localTransform).
            //
            // The win is that apply_emitted goes from O(N) Actor creates
            // per cook to O(1) — the same Actor stays alive across cooks
            // and only its instances() vector resizes / updates in place.
            // At 3000+ particles the slow path was eating ~25 ms/cook;
            // here it drops to a handful of ms.
            struct InstanceEntry
            {
                Vec3 translate{0.0f};
                // Quaternion (wxyz). Identity by default.
                Vec4 rotation{1.0f, 0.0f, 0.0f, 0.0f};
                Vec3 scale{1.0f};
                // Per-instance albedo tint. White = no override.
                Vec3 tint{1.0f, 1.0f, 1.0f};
                bool hasTint = false;
            };
            std::vector<InstanceEntry> instances;
            // Per-instance albedo tint pulled from the template's `Cd`
            // attribute (or anywhere upstream that wants to vary shading
            // per instance). White (1,1,1) means "no override" — the slow
            // path passes through the default / glTF-source material.
            // Path tracer + rasterizer already index `materials[]` by
            // `instanceCustomIndex`, so each TLAS instance picks up its
            // own tint without shader changes.
            Vec3 tint{1.0f, 1.0f, 1.0f};
            // `true` when `tint` is meaningful (set by upstream). Lets the
            // editor distinguish "actor explicitly wants white" from
            // "actor doesn't care" so a Cd-less template doesn't clobber
            // a user-assigned material library.
            bool hasTint = false;

            // Object Output material override. When `overrideMaterial` is true
            // (the output node's `override_material` toggle), these factors
            // drive the actor's material instead of the glTF/SOP source —
            // letting any object be made glass/emissive from the node's
            // params (sliders, keyframable) with no library graph. Gated by
            // the toggle so imported materials aren't clobbered when off.
            bool overrideMaterial = false;
            Vec3 ovBaseColor{0.8f, 0.8f, 0.8f};
            float ovMetallic = 0.0f;
            float ovRoughness = 0.5f;
            Vec3 ovEmission{0.0f, 0.0f, 0.0f};
            float ovEmissionStrength = 1.0f;
            float ovTransmission = 0.0f;
            float ovIor = 1.5f;
            float ovOpacity = 1.0f;
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

            // Time-aware variant. SopGraph::cook always calls cookAt so the
            // time threads through the topo walk and into recursive subnet
            // sub-graphs uniformly. The default records the time and
            // delegates to the timeless cook() — combined with the
            // animated-param routing in paramFloat/Int/Bool/Vec3 (see
            // sop_node.cpp), every node's keyed parameters animate without
            // a per-node override. Nodes that need explicit control over
            // time-sampling (effectors, transform) override this instead.
            virtual Geometry cookAt(std::span<const Geometry *const> inputs,
                                    double time) const
            {
                m_evalTime = time;
                return cook(inputs);
            }

            // True when this node's cook can vary with time even if its
            // params and inputs are byte-identical. Default: any keyed
            // parameter makes the node time-dependent (its evaluated values
            // change with the playhead even though the channel data — and
            // therefore the parameter hash — does not). attribute_vop
            // overrides to an unconditional true (its hosted VOP kernel can
            // read @Time without any keyed host param). SopGraph::cook mixes
            // the cook time into a time-dependent node's cache key and
            // propagates the flag downstream — returning false while the
            // cook varies with time poisons the cook cache across frames.
            virtual bool isTimeDependent() const;

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
            // of hand-walking m_params. When a parameter carries keyframe
            // channels these sample it at the cook's evaluation time (set by
            // the base cookAt), so plain cook() implementations animate
            // without explicit time plumbing.
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

        protected:
            // Nodes that override cookAt (and therefore skip the base's
            // time capture) call this first so the constant param getters
            // keep sampling keyed channels at the right playhead.
            void setEvalTime(double t) const { m_evalTime = t; }

        private:
            std::vector<Parameter> m_params;
            // Evaluation time recorded by the base cookAt so the constant
            // param getters can sample keyed channels at the playhead.
            // mutable: cook() is const but the time is per-invocation state.
            // Safe — SopGraph's node walk is single-threaded (parallelism
            // lives inside individual cooks, which only read this).
            mutable double m_evalTime = 0.0;
            float m_posX = 0.0f;
            float m_posY = 0.0f;
        };
    }
}
