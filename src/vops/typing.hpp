#pragma once

// Type inference for VOP graphs.
//
// VOPs are largely overloaded between Float and Vec3 — `add`, `clamp`, `fit`,
// `mix`, etc. all promote to vec3 if any input is vec3. The CPU evaluator
// used to do this with runtime variant inspection (`asFloat(v)` silently
// takes `vec.x` from a Vec3); the GPU emitter used a local `anyVec3`
// heuristic on its input slots. Those two paths produced different
// behaviour when graphs were ambiguous — a `geo_input.P → fit →
// geo_output.Cd` graph gave `vec3(P.x)` on CPU but a proper component-
// wise `vec3` on GPU.
//
// This module replaces both with a single graph-wide inference pass that
// runs once per compile and produces a port-keyed `Type` map. Both the
// emitter and the evaluator consume that map, so by construction they
// agree on every node's I/O types.
//
// Inference is Hindley-Milner-lite: each node declares a `Signature` with
// `TypeVar`s on polymorphic ports. The inferer instantiates fresh vars
// per node, walks edges in topo order propagating types, and resolves
// each var to the widest concrete type it was seen as. Promotion rule:
// Float ≤ Vec3 (a scalar wired into a vec3 port splats; the inferred
// type is Vec3). Type errors are non-fatal — unresolved vars default to
// Float, matching Houdini's behaviour for orphan-input nodes.

#include "../graph/connection.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tracey
{
    namespace vops
    {
        class VopGraph;

        // Concrete value-shape. Maps 1:1 onto the runtime `Value` variant's
        // alternatives + the GPU emitter's GLSL types. The lattice for
        // promotion is Int ≤ Float ≤ Vec3.
        enum class TypeKind : uint8_t
        {
            Float = 0,
            Int   = 1,
            Vec3  = 2,
        };

        const char *typeKindName(TypeKind k);

        // Either a concrete `TypeKind` or a polymorphic variable index
        // (`varId` is meaningful iff `isVar`). Signatures use the same
        // struct, with `isVar=true` denoting "this slot's type is bound
        // to whatever the unifier resolves variable `varId` to during
        // inference".
        struct Type
        {
            bool isVar = false;
            TypeKind kind = TypeKind::Float;
            uint32_t varId = 0;

            static Type concrete(TypeKind k) { Type t; t.kind = k; return t; }
            static Type var(uint32_t id)     { Type t; t.isVar = true; t.varId = id; return t; }

            bool isConcrete() const { return !isVar; }
        };

        // Per-node type signature. Variables are shared across input +
        // output by `varId` — e.g. `fit` declares all six ports as
        // `Type::var(0)`, so resolving variable 0 to Vec3 makes every
        // port vec3.
        struct Signature
        {
            std::vector<Type> inputs;
            std::vector<Type> outputs;
        };

        // Lookup a node-kind's signature. Returns a sentinel "untyped"
        // signature (empty vectors) when the kind isn't registered;
        // callers treat that as "this node is opaque" and emit Float
        // defaults for any consumer of its outputs.
        const Signature *signatureForKind(std::string_view kind);

        // Port-typed result of inference. Keyed by `(nodeUid, port)` —
        // separate input and output spaces because the same `port` index
        // means different things on either side.
        struct PortKey
        {
            size_t nodeUid;
            uint32_t port;
            bool     isOutput;

            bool operator==(const PortKey &o) const
            {
                return nodeUid == o.nodeUid && port == o.port && isOutput == o.isOutput;
            }
        };

        struct PortKeyHash
        {
            size_t operator()(const PortKey &k) const noexcept
            {
                // Mix the three fields with FNV-1a so we don't lean on
                // a default identity hash that's degenerate for sequential
                // uids.
                uint64_t h = 0xcbf29ce484222325ULL;
                auto mix = [&](uint64_t v) {
                    for (int i = 0; i < 8; ++i)
                    {
                        h ^= (v & 0xff);
                        h *= 0x100000001b3ULL;
                        v >>= 8;
                    }
                };
                mix(static_cast<uint64_t>(k.nodeUid));
                mix(static_cast<uint64_t>(k.port));
                mix(k.isOutput ? 1ULL : 0ULL);
                return static_cast<size_t>(h);
            }
        };

        struct TypeInferenceResult
        {
            // Inferred concrete type at each named port. Missing entries
            // mean "no inference data" — usually because the node's kind
            // doesn't have a registered signature; the caller should
            // fall back to Float in that case.
            std::unordered_map<PortKey, TypeKind, PortKeyHash> portTypes;

            // Convenience lookup. Returns Float when the port isn't
            // tracked — matches "unresolved → Float" default semantics
            // so callers don't have to branch.
            TypeKind portType(size_t nodeUid, uint32_t port, bool isOutput) const;
        };

        // Run inference over the whole graph. The graph must have run
        // compile() at least once so `topoOrder()` is populated;
        // inference re-uses that order to ensure upstream types are
        // known before each node is processed.
        TypeInferenceResult inferGraphTypes(const VopGraph &graph);
    }
}
