#include "typing.hpp"

#include "vop_graph.hpp"
#include "vop_node.hpp"

#include <algorithm>
#include <stdexcept>

namespace tracey
{
    namespace vops
    {
        const char *typeKindName(TypeKind k)
        {
            switch (k)
            {
            case TypeKind::Float: return "float";
            case TypeKind::Int:   return "int";
            case TypeKind::Vec3:  return "vec3";
            }
            return "?";
        }

        TypeKind TypeInferenceResult::portType(size_t nodeUid, uint32_t port, bool isOutput) const
        {
            auto it = portTypes.find(PortKey{nodeUid, port, isOutput});
            if (it == portTypes.end()) return TypeKind::Float;
            return it->second;
        }

        namespace
        {
            // Lattice join. Int ≤ Float ≤ Vec3. A scalar wired into a
            // vec3 port splats; the inferred type is Vec3. Two scalars
            // stay scalar. Two vec3s stay vec3. Used both during
            // variable resolution and to pick a node's output type
            // from its inputs when the signature ties them via a Var.
            TypeKind widen(TypeKind a, TypeKind b)
            {
                auto rank = [](TypeKind k) {
                    switch (k)
                    {
                    case TypeKind::Int:   return 0;
                    case TypeKind::Float: return 1;
                    case TypeKind::Vec3:  return 2;
                    }
                    return 1;
                };
                return rank(a) >= rank(b) ? a : b;
            }

            // Shorthand for signature construction. Each `Tn` is a
            // type-variable index; concrete types are spelled out.
            const Type F = Type::concrete(TypeKind::Float);
            const Type I = Type::concrete(TypeKind::Int);
            const Type V = Type::concrete(TypeKind::Vec3);
            Type T(uint32_t id) { return Type::var(id); }

            // Per-kind signature table. Built once at first lookup.
            // Polymorphic nodes (any op that promotes scalar → vec3
            // component-wise) share a single TypeVar across the
            // related ports; concrete nodes use direct TypeKinds.
            //
            // The signatures here drive both CPU evaluation and GLSL
            // emission — the previous behaviour-divergence between the
            // two paths came from each path doing its own ad-hoc
            // promotion guesswork on the same graph, so we collapse
            // them onto one source of truth.
            const std::unordered_map<std::string, Signature> &table()
            {
                static const auto sigs = [] {
                    std::unordered_map<std::string, Signature> m;

                    // ── Unified Geometry I/O nodes ──────────────────
                    // geo_input has one output port per standard
                    // attribute. Order MUST match the port order in
                    // GeoInputVop::ports():  vec3 P, N, Cd, uv, v,
                    // force, then float Alpha, pscale, age, life,
                    // ptnum. Keep this list in lockstep with the
                    // kVecPorts / kFloatPorts / kInputOnlyFloatPorts
                    // arrays in src/vops/nodes/geo_io_vops.cpp — if
                    // ports are added there, mirror them here.
                    m["geo_input"] = { {}, {
                        V, V, V, V, V, V,   // P, N, Cd, uv, v, force
                        F, F,               // Alpha, pscale
                        F, F, F             // age, life, ptnum
                    }};
                    // geo_output has one input port per writable
                    // attribute (6 vec3 + 2 float). No outputs.
                    m["geo_output"] = { {
                        V, V, V, V, V, V,   // P, N, Cd, uv, v, force
                        F, F                // Alpha, pscale
                    }, {} };

                    // ── Constants ────────────────────────────────────
                    m["constant_float"] = {{}, {F}};
                    m["constant_vec3"]  = {{}, {V}};

                    // ── Polymorphic binary math ──────────────────────
                    // a, b, out share TypeVar(0). A scalar+vec3 mix
                    // promotes both inputs (and the output) to Vec3 via
                    // the lattice join.
                    Signature poly2{ {T(0), T(0)}, {T(0)} };
                    m["add"]      = poly2;
                    m["subtract"] = poly2;
                    m["multiply"] = poly2;
                    m["divide"]   = poly2;
                    m["min"]      = poly2;
                    m["max"]      = poly2;

                    // ── mix / clamp: 3 args, all same type ───────────
                    m["mix"]   = { {T(0), T(0), T(0)}, {T(0)} };
                    m["clamp"] = { {T(0), T(0), T(0)}, {T(0)} };

                    // ── fit: 5 args, all same type. The polymorphism
                    // is the whole point — a vec3 input remaps each
                    // component independently against the bounds.
                    m["fit"] = { {T(0), T(0), T(0), T(0), T(0)}, {T(0)} };

                    // ── rand: scalar → scalar. Seeding with a vec3
                    // makes no sense; we keep it strictly Float so the
                    // unifier rejects vec3 connections instead of
                    // silently taking .x.
                    m["rand"] = { {F}, {F} };

                    // ── Unary builtins. Output type follows input. ───
                    Signature unary{ {T(0)}, {T(0)} };
                    m["abs"]    = unary;
                    m["negate"] = unary;
                    m["sign"]   = unary;
                    m["floor"]  = unary;
                    m["ceil"]   = unary;
                    m["fract"]  = unary;
                    m["sqrt"]   = unary;
                    m["sin"]    = unary;
                    m["cos"]    = unary;

                    // ── Vector ops with concrete I/O ─────────────────
                    m["length"]    = { {V},    {F} };
                    m["distance"]  = { {V, V}, {F} };
                    m["dot"]       = { {V, V}, {F} };
                    m["cross"]     = { {V, V}, {V} };
                    m["normalize"] = { {V},    {V} };
                    m["make_vec3"] = { {F, F, F}, {V} };
                    m["split_vec3"] = { {V}, {F, F, F} };

                    // ── Displacement ─────────────────────────────────
                    // displace_along_normal: in P (vec3), in N (vec3), scalar amount → vec3
                    m["displace_along_normal"] = { {V, V, F}, {V} };
                    // displace: in P (vec3) + offset (vec3) → vec3
                    m["displace"] = { {V, V}, {V} };

                    // ── Noise: P (vec3) input, scalar output ──────────
                    m["noise_perlin"]     = { {V}, {F} };
                    m["noise_simplex"]    = { {V}, {F} };
                    m["noise_worley"]     = { {V}, {F} };
                    m["noise_fbm"]        = { {V}, {F} };
                    m["noise_turbulence"] = { {V}, {F} };
                    m["noise_ridged"]     = { {V}, {F} };
                    // Vec3 noise produces a vec3 (e.g. for curl-style fields).
                    m["noise_vec3"]       = { {V}, {V} };
                    m["noise_curl"]       = { {V}, {V} };

                    return m;
                }();
                return sigs;
            }

            // Bind a TypeVar id to a concrete TypeKind. If the var was
            // previously bound to something else, take the lattice
            // join (widening). Returns the resolved kind.
            TypeKind bindVar(std::unordered_map<uint32_t, TypeKind> &vars,
                             uint32_t id, TypeKind k)
            {
                auto it = vars.find(id);
                if (it == vars.end()) { vars[id] = k; return k; }
                const TypeKind merged = widen(it->second, k);
                it->second = merged;
                return merged;
            }
        }

        const Signature *signatureForKind(std::string_view kind)
        {
            const auto &t = table();
            auto it = t.find(std::string(kind));
            return it == t.end() ? nullptr : &it->second;
        }

        TypeInferenceResult inferGraphTypes(const VopGraph &graph)
        {
            TypeInferenceResult result;

            // The caller (typically VopGraph::compile()) is responsible
            // for having built topoOrder() already. Calling compile()
            // here would recurse — compile() invokes us as part of its
            // own work — so we trust the precondition and bail clean
            // if the order is empty.

            // Per-node remapped variable IDs. Each visit instantiates
            // fresh integers so variables of different nodes don't
            // collide (`add`'s T(0) is distinct from `multiply`'s).
            // We then bind those IDs against the lattice as connections
            // are unified, and finally resolve each port's declared
            // signature type to a concrete TypeKind.
            std::unordered_map<uint32_t, TypeKind> vars;
            uint32_t nextVarBase = 1;

            for (size_t uid : graph.topoOrder())
            {
                const VopNode *node = graph.findNode(uid);
                if (!node) continue;

                const Signature *sig = signatureForKind(node->kind());
                if (!sig) continue;  // opaque node — no inference data

                // Remap variable ids so this node's vars don't alias any
                // other node's. `remap[localId] = globalId`.
                std::unordered_map<uint32_t, uint32_t> remap;
                auto globalize = [&](const Type &t) -> Type {
                    if (!t.isVar) return t;
                    auto it = remap.find(t.varId);
                    if (it != remap.end()) return Type::var(it->second);
                    const uint32_t g = nextVarBase++;
                    remap[t.varId] = g;
                    return Type::var(g);
                };

                std::vector<Type> inputs;
                inputs.reserve(sig->inputs.size());
                for (const Type &t : sig->inputs) inputs.push_back(globalize(t));
                std::vector<Type> outputs;
                outputs.reserve(sig->outputs.size());
                for (const Type &t : sig->outputs) outputs.push_back(globalize(t));

                // Unify each input slot against its upstream output's
                // already-inferred type. Concrete↔concrete: lattice
                // join (widening, so a Float wired into a Vec3 port
                // widens that var to Vec3). Var↔concrete: bind the var
                // to the concrete type's join with whatever it was.
                for (size_t i = 0; i < inputs.size(); ++i)
                {
                    auto src = graph.incomingTo(uid, i);
                    if (!src) continue;  // unconnected — leave default
                    const TypeKind upstream = result.portType(src->first, static_cast<uint32_t>(src->second), /*isOutput=*/true);

                    Type &slot = inputs[i];
                    if (slot.isVar)
                    {
                        bindVar(vars, slot.varId, upstream);
                    }
                    else
                    {
                        // Concrete declared port type. Widen with the
                        // upstream type and remember the slot stays at
                        // its declared kind — the GPU emitter / CPU
                        // evaluator will cast on the caller side.
                        slot.kind = widen(slot.kind, upstream);
                    }
                }

                // Resolve and store every port's final type — both
                // inputs and outputs. Inputs are recorded so the
                // emitter / evaluator can look up the type the kernel
                // actually sees after promotion, and so that downstream
                // consumers reading via `incomingTo` find a typed
                // upstream output.
                auto resolveType = [&](const Type &t) -> TypeKind {
                    if (!t.isVar) return t.kind;
                    auto it = vars.find(t.varId);
                    if (it == vars.end()) return TypeKind::Float;  // orphan default
                    return it->second;
                };
                for (size_t i = 0; i < inputs.size(); ++i)
                {
                    result.portTypes[{uid, static_cast<uint32_t>(i), false}] = resolveType(inputs[i]);
                }
                for (size_t o = 0; o < outputs.size(); ++o)
                {
                    result.portTypes[{uid, static_cast<uint32_t>(o), true}] = resolveType(outputs[o]);
                }
            }

            // Second pass to widen output types for nodes whose
            // outputs share a TypeVar with their inputs — after the
            // first pass we know the var's final kind, but the
            // output's resolved type was based on the value at the
            // moment of visit. Re-resolve so a later input bind
            // (e.g. via backward-flowing promotion through a wider
            // graph) propagates to the same node's outputs too.
            // In a strict feed-forward unifier this is a no-op; we
            // keep it as a safety net for graphs where the topo
            // order doesn't capture all the unifications in one pass.
            nextVarBase = 1;
            for (size_t uid : graph.topoOrder())
            {
                const VopNode *node = graph.findNode(uid);
                if (!node) continue;
                const Signature *sig = signatureForKind(node->kind());
                if (!sig) continue;
                std::unordered_map<uint32_t, uint32_t> remap;
                auto globalize = [&](const Type &t) -> Type {
                    if (!t.isVar) return t;
                    auto it = remap.find(t.varId);
                    if (it != remap.end()) return Type::var(it->second);
                    const uint32_t g = nextVarBase++;
                    remap[t.varId] = g;
                    return Type::var(g);
                };
                for (size_t o = 0; o < sig->outputs.size(); ++o)
                {
                    const Type t = globalize(sig->outputs[o]);
                    if (t.isVar)
                    {
                        auto it = vars.find(t.varId);
                        if (it != vars.end())
                        {
                            result.portTypes[{uid, static_cast<uint32_t>(o), true}] = it->second;
                        }
                    }
                }
            }

            return result;
        }
    }
}
