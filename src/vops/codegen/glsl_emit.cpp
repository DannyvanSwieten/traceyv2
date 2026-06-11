// VopGraph → GLSL compute shader emitter. See header for the design
// rationale + per-shader assumptions (one SSBO per touched attribute,
// param vec4 SSBO bound after attributes, push-constant pointCount).
//
// Implementation shape:
//   • Walk graph in topo order (assumes graph.compile() already ran).
//   • For each node kind, dispatch to a small inline emitter that
//     appends a few lines to `state.body`.
//   • Each output port (uid, port) gets a stable local var name
//     `slot_<uid>_<port>` and a tracked GpuType for downstream casts.
//   • geo_input / geo_output ports register their attribute in
//     `state.attrs` so the result reports it back to the dispatcher.
//   • Each parameter the emitter reads gets allocated a vec4 slot;
//     the CPU writes those slots per cook (no recompile on edit).
//
// Phase 1 supports the ~30 most common VOP node kinds (geo I/O, math,
// vec ops, noise basics, displacement). Unsupported kinds get
// `// unsupported: <kind>` in the body + listed in `result.unsupported`
// so the caller can fall back to the CPU evaluator until coverage
// catches up.

#include "glsl_emit.hpp"

#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../geo_io_ports.hpp"

#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>

namespace tracey
{
    namespace vops
    {
        namespace codegen
        {
            namespace
            {
                struct SlotInfo
                {
                    std::string name;  // GLSL local variable name
                    GpuType type;
                };

                // Cap on emitted node-source lines as a sanity belt. A
                // pathological graph with thousands of nodes would still
                // compile but we'd rather fail loudly than silently emit
                // a 10MB shader.
                constexpr size_t kMaxNodes = 2048;

                struct EmitState
                {
                    // (nodeUid << 32) | port → slot info
                    std::unordered_map<uint64_t, SlotInfo> slots;
                    // attribute name → index into result.attrs. Indices,
                    // not pointers: a later push_back into result.attrs
                    // can grow the vector and invalidate any stored
                    // pointers — and several emitters (notably
                    // emitGeoInput) push_back multiple attrs back-to-back
                    // before any later lookup reaches the cached entry.
                    std::unordered_map<std::string, size_t> attrIndex;

                    EmitResult result;

                    std::ostringstream body;
                };

                uint64_t slotKey(size_t uid, size_t port)
                {
                    return (static_cast<uint64_t>(uid) << 32) |
                           static_cast<uint64_t>(port & 0xFFFFFFFFu);
                }

                std::string slotName(size_t uid, size_t port)
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "slot_%zu_%zu", uid, port);
                    return buf;
                }

                const char *gpuTypeStr(GpuType t)
                {
                    switch (t)
                    {
                    case GpuType::Float: return "float";
                    case GpuType::Vec3:  return "vec3";
                    case GpuType::Int:   return "int";
                    }
                    return "float";
                }

                std::string defaultExpr(GpuType t)
                {
                    switch (t)
                    {
                    case GpuType::Float: return "0.0";
                    case GpuType::Vec3:  return "vec3(0.0)";
                    case GpuType::Int:   return "0";
                    }
                    return "0.0";
                }

                // Coerce an expression of source type `from` into a target
                // type `to`. Mirrors the CPU asFloat / asVec3 conversions in
                // math_vops.cpp (float→vec3 splats, vec3→float takes .x).
                std::string castExpr(const std::string &expr, GpuType from, GpuType to)
                {
                    if (from == to) return expr;
                    if (to == GpuType::Vec3)
                    {
                        if (from == GpuType::Float) return "vec3(" + expr + ")";
                        if (from == GpuType::Int)   return "vec3(float(" + expr + "))";
                    }
                    if (to == GpuType::Float)
                    {
                        if (from == GpuType::Vec3) return "(" + expr + ").x";
                        if (from == GpuType::Int)  return "float(" + expr + ")";
                    }
                    if (to == GpuType::Int)
                    {
                        if (from == GpuType::Vec3)  return "int((" + expr + ").x)";
                        if (from == GpuType::Float) return "int(" + expr + ")";
                    }
                    return expr;
                }

                // Read an input port: resolve the upstream slot if wired,
                // else fall back to the type's zero value. Returns the
                // (expr, type) pair the caller can fold into its emitted
                // statement.
                struct InputRead
                {
                    std::string expr;
                    GpuType type;
                };
                InputRead readInput(const VopGraph &graph,
                                    const EmitState &state,
                                    size_t nodeUid,
                                    size_t inputPortIdx,
                                    GpuType fallbackType)
                {
                    auto src = graph.incomingTo(nodeUid, inputPortIdx);
                    if (!src) return {defaultExpr(fallbackType), fallbackType};
                    auto it = state.slots.find(slotKey(src->first, src->second));
                    if (it == state.slots.end())
                        return {defaultExpr(fallbackType), fallbackType};
                    return {it->second.name, it->second.type};
                }

                // Register / look-up a point attribute SSBO. Repeated calls
                // for the same name return the same binding and merge the
                // read/write flags.
                uint32_t ensureAttr(EmitState &state,
                                    const std::string &name,
                                    GpuType type,
                                    bool read, bool write)
                {
                    auto it = state.attrIndex.find(name);
                    if (it != state.attrIndex.end())
                    {
                        AttrBinding &existing = state.result.attrs[it->second];
                        existing.read  = existing.read  || read;
                        existing.write = existing.write || write;
                        return existing.binding;
                    }
                    AttrBinding b;
                    b.name = name;
                    b.type = type;
                    b.binding = static_cast<uint32_t>(state.result.attrs.size());
                    b.read = read;
                    b.write = write;
                    const size_t idx = state.result.attrs.size();
                    state.result.attrs.push_back(std::move(b));
                    state.attrIndex[name] = idx;
                    return state.result.attrs[idx].binding;
                }

                // Allocate a vec4 slot for a node parameter. The CPU writes
                // the live param value into this slot every cook before
                // dispatch.
                uint32_t addParam(EmitState &state, size_t nodeUid,
                                  const std::string &paramName, GpuType type)
                {
                    ParamSlot p;
                    p.nodeUid = nodeUid;
                    p.paramName = paramName;
                    p.type = type;
                    p.slot = state.result.paramSlotCount++;
                    state.result.params.push_back(std::move(p));
                    return state.result.params.back().slot;
                }

                // Emit `<type> slotName = <expr>;` and register the slot in
                // the state so downstream nodes can read it.
                void emitDecl(EmitState &state, size_t nodeUid, size_t outputPort,
                              GpuType type, const std::string &expr)
                {
                    const std::string name = slotName(nodeUid, outputPort);
                    state.body << "    " << gpuTypeStr(type) << " " << name
                               << " = " << expr << ";\n";
                    state.slots[slotKey(nodeUid, outputPort)] = {name, type};
                }

                // ── Per-node emitters ────────────────────────────────────
                //
                // Each handler reads what it needs from `node` + `graph`,
                // appends its statement(s) to `state.body`, and registers
                // its output slot via emitDecl(). Returns true if the
                // node was recognised; false → caller adds to unsupported.

                // Binary math: dispatch on the operator string, infer
                // output type as "Vec3 if any input is Vec3, else Float".
                bool emitBinaryOp(EmitState &state, const VopGraph &graph,
                                  const VopNode &node, const char *op,
                                  bool divisionZeroGuard = false)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    const GpuType outType = (a.type == GpuType::Vec3 || b.type == GpuType::Vec3)
                                          ? GpuType::Vec3 : GpuType::Float;
                    const std::string aE = castExpr(a.expr, a.type, outType);
                    const std::string bE = castExpr(b.expr, b.type, outType);
                    std::string expr;
                    if (divisionZeroGuard)
                    {
                        // Match the CPU evaluator: divide-by-zero yields 0
                        // rather than NaN. mix(x, 0, isZero(b)) does it
                        // branch-free.
                        if (outType == GpuType::Vec3)
                        {
                            expr = "mix(" + aE + " / max(abs(" + bE + "), vec3(1e-30)) * sign(" + bE + "), vec3(0.0), step(abs(" + bE + "), vec3(0.0)))";
                        }
                        else
                        {
                            expr = "(abs(" + bE + ") > 0.0 ? " + aE + " / " + bE + " : 0.0)";
                        }
                    }
                    else
                    {
                        expr = "(" + aE + " " + op + " " + bE + ")";
                    }
                    emitDecl(state, node.uid(), 0, outType, expr);
                    return true;
                }

                // Float-returning binary op (length / dot / distance).
                bool emitDot(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             "dot(" + castExpr(a.expr, a.type, GpuType::Vec3) + ", " +
                                       castExpr(b.expr, b.type, GpuType::Vec3) + ")");
                    return true;
                }
                bool emitCross(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Vec3,
                             "cross(" + castExpr(a.expr, a.type, GpuType::Vec3) + ", " +
                                         castExpr(b.expr, b.type, GpuType::Vec3) + ")");
                    return true;
                }
                bool emitDistance(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             "distance(" + castExpr(a.expr, a.type, GpuType::Vec3) + ", " +
                                            castExpr(b.expr, b.type, GpuType::Vec3) + ")");
                    return true;
                }
                bool emitLength(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             "length(" + castExpr(v.expr, v.type, GpuType::Vec3) + ")");
                    return true;
                }
                bool emitNormalize(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    // Guarded normalize: zero-length input passes through as
                    // zero (matches the CPU evaluator). GLSL `normalize`
                    // of a zero vector is undefined → divide-by-zero guard.
                    const std::string e = castExpr(v.expr, v.type, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Vec3,
                             "(dot(" + e + ", " + e + ") > 0.0 ? normalize(" + e + ") : vec3(0.0))");
                    return true;
                }
                bool emitMix(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    auto t = readInput(graph, state, node.uid(), 2, GpuType::Float);
                    const GpuType outType = (a.type == GpuType::Vec3 || b.type == GpuType::Vec3)
                                          ? GpuType::Vec3 : GpuType::Float;
                    const std::string aE = castExpr(a.expr, a.type, outType);
                    const std::string bE = castExpr(b.expr, b.type, outType);
                    const std::string tE = castExpr(t.expr, t.type, GpuType::Float);
                    emitDecl(state, node.uid(), 0, outType,
                             "mix(" + aE + ", " + bE + ", " + tE + ")");
                    return true;
                }
                bool emitMinMax(EmitState &state, const VopGraph &graph, const VopNode &node,
                                const char *fn)
                {
                    auto a = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto b = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    const GpuType outType = (a.type == GpuType::Vec3 || b.type == GpuType::Vec3)
                                          ? GpuType::Vec3 : GpuType::Float;
                    emitDecl(state, node.uid(), 0, outType,
                             std::string(fn) + "(" + castExpr(a.expr, a.type, outType) + ", " +
                                                     castExpr(b.expr, b.type, outType) + ")");
                    return true;
                }
                bool emitClamp(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v  = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto lo = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    auto hi = readInput(graph, state, node.uid(), 2, GpuType::Float);
                    const GpuType outType = (v.type == GpuType::Vec3 || lo.type == GpuType::Vec3 ||
                                             hi.type == GpuType::Vec3)
                                          ? GpuType::Vec3 : GpuType::Float;
                    emitDecl(state, node.uid(), 0, outType,
                             "clamp(" + castExpr(v.expr,  v.type,  outType) + ", " +
                                         castExpr(lo.expr, lo.type, outType) + ", " +
                                         castExpr(hi.expr, hi.type, outType) + ")");
                    return true;
                }
                // Remap `value` from [src_min, src_max] to [dst_min, dst_max].
                // Mirrors FitVop::evaluate's CPU formula: t = (v - sa) / (sb - sa);
                // out = da + (db - da) * t. Guard the zero-span case with a
                // 1e-12 epsilon to match the CPU branch — Houdini's fit
                // VOP returns dst_min when src_min == src_max, which is
                // what the t=0 path here gives us after the mix.
                bool emitFit(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v  = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto sa = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    auto sb = readInput(graph, state, node.uid(), 2, GpuType::Float);
                    auto da = readInput(graph, state, node.uid(), 3, GpuType::Float);
                    auto db = readInput(graph, state, node.uid(), 4, GpuType::Float);
                    // Output type follows any vec3 input — if value or
                    // either bound is a vec3, the whole expression
                    // promotes (matches the CPU behaviour where vec3
                    // inputs propagate component-wise via mix()).
                    const GpuType outType =
                        (v.type == GpuType::Vec3 || sa.type == GpuType::Vec3 ||
                         sb.type == GpuType::Vec3 || da.type == GpuType::Vec3 ||
                         db.type == GpuType::Vec3) ? GpuType::Vec3 : GpuType::Float;
                    const std::string vE  = castExpr(v.expr,  v.type,  outType);
                    const std::string saE = castExpr(sa.expr, sa.type, outType);
                    const std::string sbE = castExpr(sb.expr, sb.type, outType);
                    const std::string daE = castExpr(da.expr, da.type, outType);
                    const std::string dbE = castExpr(db.expr, db.type, outType);
                    // `(sb - sa)` could be zero — divide protected by a
                    // max() with epsilon so a degenerate fit doesn't
                    // produce NaN/Inf that poisons the rest of the kernel.
                    const std::string span = "max(abs(" + sbE + " - " + saE +
                                             "), " +
                                             (outType == GpuType::Vec3
                                                  ? std::string("vec3(1e-12)")
                                                  : std::string("1e-12")) + ")";
                    const std::string t    = "((" + vE + " - " + saE + ") / " + span + ")";
                    emitDecl(state, node.uid(), 0, outType,
                             daE + " + (" + dbE + " - " + daE + ") * " + t);
                    return true;
                }

                // Seed → [0, 1) float. Same xorshift32-style hash as
                // RandVop::evaluate on the CPU side so a graph that switches
                // between CPU + GPU dispatch produces the same noise pattern.
                bool emitRand(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto s = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    // floatBitsToUint mirrors the CPU memcpy(&bits, &seed)
                    // exactly. The three magic constants are the same ones
                    // RandVop uses; the AND with 0x00ffffff + the divide by
                    // 0x01000000 keep the result in [0, 1).
                    const std::string seed = s.expr;
                    state.body
                        << "    uint __rb_" << node.uid() << " = floatBitsToUint(float(" << seed << "));\n"
                        << "    __rb_" << node.uid() << " = __rb_" << node.uid()
                        << " * 2654435761u + 374761393u;\n"
                        << "    __rb_" << node.uid() << " ^= __rb_" << node.uid() << " >> 13;\n"
                        << "    __rb_" << node.uid() << " *= 0x85ebca6bu;\n"
                        << "    __rb_" << node.uid() << " ^= __rb_" << node.uid() << " >> 16;\n";
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             "float(__rb_" + std::to_string(node.uid()) +
                             " & 0x00ffffffu) / float(0x01000000u)");
                    return true;
                }

                // Unary scalar-or-vec3 op via a one-arg GLSL builtin.
                bool emitUnary(EmitState &state, const VopGraph &graph, const VopNode &node,
                               const char *fn)
                {
                    auto v = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    emitDecl(state, node.uid(), 0, v.type,
                             std::string(fn) + "(" + v.expr + ")");
                    return true;
                }
                bool emitNegate(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    emitDecl(state, node.uid(), 0, v.type, "-(" + v.expr + ")");
                    return true;
                }
                bool emitMakeVec3(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto x = readInput(graph, state, node.uid(), 0, GpuType::Float);
                    auto y = readInput(graph, state, node.uid(), 1, GpuType::Float);
                    auto z = readInput(graph, state, node.uid(), 2, GpuType::Float);
                    emitDecl(state, node.uid(), 0, GpuType::Vec3,
                             "vec3(" + castExpr(x.expr, x.type, GpuType::Float) + ", " +
                                       castExpr(y.expr, y.type, GpuType::Float) + ", " +
                                       castExpr(z.expr, z.type, GpuType::Float) + ")");
                    return true;
                }
                bool emitSplitVec3(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto v = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const std::string vE = castExpr(v.expr, v.type, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Float, "(" + vE + ").x");
                    emitDecl(state, node.uid(), 1, GpuType::Float, "(" + vE + ").y");
                    emitDecl(state, node.uid(), 2, GpuType::Float, "(" + vE + ").z");
                    return true;
                }

                // constant_*: declares a vec4 slot in the param SSBO so
                // the CPU can edit the value without recompiling. .x for
                // float; .xyz for vec3.
                bool emitConstantFloat(EmitState &state, const VopNode &node)
                {
                    const uint32_t slot = addParam(state, node.uid(), "value", GpuType::Float);
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             "params.data[" + std::to_string(slot) + "].x");
                    return true;
                }
                bool emitConstantVec3(EmitState &state, const VopNode &node)
                {
                    const uint32_t slot = addParam(state, node.uid(), "value", GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Vec3,
                             "params.data[" + std::to_string(slot) + "].xyz");
                    return true;
                }

                // Noise nodes — frequency/amplitude/seed pack into the
                // param SSBO; the kernel-side perlin/simplex functions
                // live in the preamble (see emitGlsl below).
                bool emitNoisePerlin(EmitState &state, const VopGraph &graph,
                                     const VopNode &node, bool simplex)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const uint32_t freqSlot = addParam(state, node.uid(), "frequency", GpuType::Float);
                    const uint32_t ampSlot  = addParam(state, node.uid(), "amplitude", GpuType::Float);
                    const uint32_t seedSlot = addParam(state, node.uid(), "seed",      GpuType::Int);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    const std::string call = simplex ? "vop_simplex(" : "vop_perlin(";
                    emitDecl(state, node.uid(), 0, GpuType::Float,
                             call +
                                "vop_seed_shift(" + pE +
                                " * params.data[" + std::to_string(freqSlot) + "].x, " +
                                "int(params.data[" + std::to_string(seedSlot) + "].x))) " +
                                "* params.data[" + std::to_string(ampSlot) + "].x");
                    return true;
                }
                // Octaved Perlin variants (fBm / turbulence / ridged).
                // All three share the same six-parameter shape and
                // generate identical setup; only the kernel function
                // they call differs.
                bool emitOctavedNoise(EmitState &state, const VopGraph &graph,
                                      const VopNode &node, const char *fn)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const uint32_t freqSlot = addParam(state, node.uid(), "frequency",  GpuType::Float);
                    const uint32_t ampSlot  = addParam(state, node.uid(), "amplitude",  GpuType::Float);
                    const uint32_t octSlot  = addParam(state, node.uid(), "octaves",    GpuType::Int);
                    const uint32_t lacSlot  = addParam(state, node.uid(), "lacunarity", GpuType::Float);
                    const uint32_t gainSlot = addParam(state, node.uid(), "gain",       GpuType::Float);
                    const uint32_t seedSlot = addParam(state, node.uid(), "seed",       GpuType::Int);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    std::ostringstream e;
                    e << fn << "(vop_seed_shift(" << pE
                      << " * params.data[" << freqSlot << "].x, "
                      << "int(params.data[" << seedSlot << "].x)), "
                      << "int(params.data[" << octSlot << "].x), "
                      << "params.data[" << lacSlot << "].x, "
                      << "params.data[" << gainSlot << "].x) "
                      << "* params.data[" << ampSlot << "].x";
                    emitDecl(state, node.uid(), 0, GpuType::Float, e.str());
                    return true;
                }

                // noise_curl: 4 params (freq, amp, eps, seed). Eps is
                // clamped to a small floor on emit so the CPU/GPU
                // parity holds against the same "max(1e-5, eps)" rule
                // the CPU evaluator applies.
                bool emitNoiseCurl(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const uint32_t freqSlot = addParam(state, node.uid(), "frequency", GpuType::Float);
                    const uint32_t ampSlot  = addParam(state, node.uid(), "amplitude", GpuType::Float);
                    const uint32_t epsSlot  = addParam(state, node.uid(), "eps",       GpuType::Float);
                    const uint32_t seedSlot = addParam(state, node.uid(), "seed",      GpuType::Int);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    std::ostringstream e;
                    e << "vop_noise_curl("
                      << pE << " * params.data[" << freqSlot << "].x, "
                      << "int(params.data[" << seedSlot << "].x), "
                      << "max(1e-5, params.data[" << epsSlot << "].x)) "
                      << "* params.data[" << ampSlot << "].x";
                    emitDecl(state, node.uid(), 0, GpuType::Vec3, e.str());
                    return true;
                }

                // noise_worley: 3 params (freq, amp, seed). Same shape as
                // perlin/simplex on the call site so the user can swap
                // between them. Output is non-negative (distance to
                // nearest jittered feature point).
                bool emitNoiseWorley(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const uint32_t freqSlot = addParam(state, node.uid(), "frequency", GpuType::Float);
                    const uint32_t ampSlot  = addParam(state, node.uid(), "amplitude", GpuType::Float);
                    const uint32_t seedSlot = addParam(state, node.uid(), "seed",      GpuType::Int);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    std::ostringstream e;
                    e << "vop_worley_f1("
                      << pE << " * params.data[" << freqSlot << "].x, "
                      << "int(params.data[" << seedSlot << "].x)) "
                      << "* params.data[" << ampSlot << "].x";
                    emitDecl(state, node.uid(), 0, GpuType::Float, e.str());
                    return true;
                }

                // noise_vec3: three decorrelated Perlin samples.
                bool emitNoiseVec3(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    const uint32_t freqSlot = addParam(state, node.uid(), "frequency", GpuType::Float);
                    const uint32_t ampSlot  = addParam(state, node.uid(), "amplitude", GpuType::Float);
                    const uint32_t seedSlot = addParam(state, node.uid(), "seed",      GpuType::Int);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    std::ostringstream e;
                    e << "vec3(vop_perlin(vop_seed_shift(" << pE
                      << " * params.data[" << freqSlot << "].x, int(params.data[" << seedSlot << "].x))), "
                      << "vop_perlin(vop_seed_shift(" << pE
                      << " * params.data[" << freqSlot << "].x, int(params.data[" << seedSlot << "].x) + 41)), "
                      << "vop_perlin(vop_seed_shift(" << pE
                      << " * params.data[" << freqSlot << "].x, int(params.data[" << seedSlot << "].x) + 83))) "
                      << "* params.data[" << ampSlot << "].x";
                    emitDecl(state, node.uid(), 0, GpuType::Vec3, e.str());
                    return true;
                }

                // Displacement helpers — typed exactly like the CPU
                // versions in displacement_vops.cpp.
                bool emitDisplaceAlongNormal(EmitState &state, const VopGraph &graph,
                                             const VopNode &node)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    auto n = readInput(graph, state, node.uid(), 1, GpuType::Vec3);
                    auto a = readInput(graph, state, node.uid(), 2, GpuType::Float);
                    const std::string pE = castExpr(p.expr, p.type, GpuType::Vec3);
                    const std::string nE = castExpr(n.expr, n.type, GpuType::Vec3);
                    const std::string aE = castExpr(a.expr, a.type, GpuType::Float);
                    // Length-guarded so zero N is a passthrough — matches CPU.
                    const std::string expr =
                        "(dot(" + nE + ", " + nE + ") > 0.0 ? " +
                        pE + " + normalize(" + nE + ") * " + aE + " : " + pE + ")";
                    emitDecl(state, node.uid(), 0, GpuType::Vec3, expr);
                    return true;
                }
                bool emitDisplace(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    auto p = readInput(graph, state, node.uid(), 0, GpuType::Vec3);
                    auto o = readInput(graph, state, node.uid(), 1, GpuType::Vec3);
                    emitDecl(state, node.uid(), 0, GpuType::Vec3,
                             castExpr(p.expr, p.type, GpuType::Vec3) + " + " +
                             castExpr(o.expr, o.type, GpuType::Vec3));
                    return true;
                }

                // ── Unified Geometry I/O ─────────────────────────────────
                //
                // The port arrays (kGeoVecPorts / kGeoFloatPorts /
                // kGeoReadOnlyFloatPorts) come from ../geo_io_ports.hpp —
                // shared with the CPU nodes in geo_io_vops.cpp and the
                // dispatcher, so the port-index contract can't drift.

                // Which (uid, outputPort) pairs feed at least one
                // downstream input? Used by emitGeoInput to dead-strip
                // unused output ports the same way orphan bind_in_* nodes
                // get stripped — a geo_input that only feeds Cd shouldn't
                // pull N / uv / v / force / Alpha / pscale into the
                // descriptor set.
                bool outputHasConsumer(const VopGraph &graph, size_t uid, size_t port)
                {
                    for (const auto &c : graph.connections())
                    {
                        if (c.fromNode == uid && c.fromPort == port) return true;
                    }
                    return false;
                }

                bool emitGeoInput(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    const size_t numVec = kGeoVecPorts.size();
                    const size_t numF   = kGeoFloatPorts.size();
                    const size_t numRO  = kGeoReadOnlyFloatPorts.size();

                    size_t portIdx = 0;
                    for (size_t i = 0; i < numVec; ++i, ++portIdx)
                    {
                        if (!outputHasConsumer(graph, node.uid(), portIdx)) continue;
                        const auto &p = kGeoVecPorts[i];
                        ensureAttr(state, p.name, GpuType::Vec3, true, false);
                        emitDecl(state, node.uid(), portIdx, GpuType::Vec3,
                                 std::string("attr_") + p.name + ".data[pi].xyz");
                    }
                    for (size_t i = 0; i < numF; ++i, ++portIdx)
                    {
                        if (!outputHasConsumer(graph, node.uid(), portIdx)) continue;
                        const auto &p = kGeoFloatPorts[i];
                        ensureAttr(state, p.name, GpuType::Float, true, false);
                        emitDecl(state, node.uid(), portIdx, GpuType::Float,
                                 std::string("attr_") + p.name + ".data[pi]");
                    }
                    // age / life are real float attributes; ptnum is the
                    // current invocation index.
                    for (size_t i = 0; i < numRO; ++i, ++portIdx)
                    {
                        if (!outputHasConsumer(graph, node.uid(), portIdx)) continue;
                        const auto &p = kGeoReadOnlyFloatPorts[i];
                        if (std::string(p.name) == "ptnum")
                        {
                            emitDecl(state, node.uid(), portIdx, GpuType::Float, "float(pi)");
                        }
                        else
                        {
                            ensureAttr(state, p.name, GpuType::Float, true, false);
                            emitDecl(state, node.uid(), portIdx, GpuType::Float,
                                     std::string("attr_") + p.name + ".data[pi]");
                        }
                    }
                    return true;
                }

                bool emitGeoOutput(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    const size_t numVec = kGeoVecPorts.size();
                    const size_t numF   = kGeoFloatPorts.size();

                    size_t portIdx = 0;
                    // Vec3 ports.
                    for (size_t i = 0; i < numVec; ++i, ++portIdx)
                    {
                        const auto &p = kGeoVecPorts[i];
                        const bool connected = graph.incomingTo(node.uid(), portIdx).has_value();
                        const bool passthrough = node.paramBool(
                            std::string("passthrough_") + p.name, true);
                        // No connection + passthrough → skip entirely.
                        // No connection + !passthrough → stamp default.
                        // Connection → write upstream value.
                        if (!connected && passthrough) continue;
                        ensureAttr(state, p.name, GpuType::Vec3, false, true);
                        std::string valueExpr;
                        if (connected)
                        {
                            auto v = readInput(graph, state, node.uid(), portIdx, GpuType::Vec3);
                            valueExpr = castExpr(v.expr, v.type, GpuType::Vec3);
                        }
                        else
                        {
                            valueExpr = p.defaultGlsl;
                        }
                        state.body << "    attr_" << p.name << ".data[pi] = vec4("
                                   << valueExpr << ", 0.0);\n";
                    }
                    // Float ports.
                    for (size_t i = 0; i < numF; ++i, ++portIdx)
                    {
                        const auto &p = kGeoFloatPorts[i];
                        const bool connected = graph.incomingTo(node.uid(), portIdx).has_value();
                        const bool passthrough = node.paramBool(
                            std::string("passthrough_") + p.name, true);
                        if (!connected && passthrough) continue;
                        ensureAttr(state, p.name, GpuType::Float, false, true);
                        std::string valueExpr;
                        if (connected)
                        {
                            auto v = readInput(graph, state, node.uid(), portIdx, GpuType::Float);
                            valueExpr = castExpr(v.expr, v.type, GpuType::Float);
                        }
                        else
                        {
                            valueExpr = p.defaultGlsl;
                        }
                        state.body << "    attr_" << p.name << ".data[pi] = "
                                   << valueExpr << ";\n";
                    }
                    return true;
                }

                // Main dispatch: returns true if `node`'s kind was handled.
                bool emitNode(EmitState &state, const VopGraph &graph, const VopNode &node)
                {
                    const std::string &k = node.kind();

                    // ── Unified Geometry I/O ─────────────────────────────
                    if (k == "geo_input")  return emitGeoInput (state, graph, node);
                    if (k == "geo_output") return emitGeoOutput(state, graph, node);

                    // ── Constants ────────────────────────────────────────
                    if (k == "constant_float") return emitConstantFloat(state, node);
                    if (k == "constant_vec3")  return emitConstantVec3 (state, node);

                    // ── Binary math ──────────────────────────────────────
                    if (k == "add")      return emitBinaryOp(state, graph, node, "+");
                    if (k == "subtract") return emitBinaryOp(state, graph, node, "-");
                    if (k == "multiply") return emitBinaryOp(state, graph, node, "*");
                    if (k == "divide")   return emitBinaryOp(state, graph, node, "/", /*divisionZeroGuard=*/true);
                    if (k == "min")      return emitMinMax(state, graph, node, "min");
                    if (k == "max")      return emitMinMax(state, graph, node, "max");
                    if (k == "mix")      return emitMix(state, graph, node);
                    if (k == "clamp")    return emitClamp(state, graph, node);
                    if (k == "fit")      return emitFit(state, graph, node);
                    if (k == "rand")     return emitRand(state, graph, node);

                    // ── Vector ───────────────────────────────────────────
                    if (k == "length")    return emitLength(state, graph, node);
                    if (k == "distance")  return emitDistance(state, graph, node);
                    if (k == "dot")       return emitDot(state, graph, node);
                    if (k == "cross")     return emitCross(state, graph, node);
                    if (k == "normalize") return emitNormalize(state, graph, node);
                    if (k == "make_vec3") return emitMakeVec3(state, graph, node);
                    if (k == "split_vec3")return emitSplitVec3(state, graph, node);

                    // ── Unary math (GLSL builtin) ────────────────────────
                    if (k == "abs")    return emitUnary(state, graph, node, "abs");
                    if (k == "negate") return emitNegate(state, graph, node);
                    if (k == "sign")   return emitUnary(state, graph, node, "sign");
                    if (k == "floor")  return emitUnary(state, graph, node, "floor");
                    if (k == "ceil")   return emitUnary(state, graph, node, "ceil");
                    if (k == "fract")  return emitUnary(state, graph, node, "fract");
                    if (k == "sqrt")   return emitUnary(state, graph, node, "sqrt");
                    if (k == "sin")    return emitUnary(state, graph, node, "sin");
                    if (k == "cos")    return emitUnary(state, graph, node, "cos");

                    // ── Noise ────────────────────────────────────────────
                    if (k == "noise_perlin")     return emitNoisePerlin(state, graph, node, false);
                    if (k == "noise_simplex")    return emitNoisePerlin(state, graph, node, true);
                    if (k == "noise_worley")     return emitNoiseWorley(state, graph, node);
                    if (k == "noise_vec3")       return emitNoiseVec3(state, graph, node);
                    if (k == "noise_curl")       return emitNoiseCurl(state, graph, node);
                    if (k == "noise_fbm")        return emitOctavedNoise(state, graph, node, "vop_noise_fbm");
                    if (k == "noise_turbulence") return emitOctavedNoise(state, graph, node, "vop_noise_turbulence");
                    if (k == "noise_ridged")     return emitOctavedNoise(state, graph, node, "vop_noise_ridged");

                    // ── Displacement ─────────────────────────────────────
                    if (k == "displace_along_normal") return emitDisplaceAlongNormal(state, graph, node);
                    if (k == "displace")              return emitDisplace(state, graph, node);

                    return false;
                }

                // GLSL preamble: perlin/simplex/seed-shift helpers shared
                // by every emitted shader. Lifted directly from Stefan
                // Gustavson's well-known reference (public-domain).
                //
                // Kept inline rather than #include'd so the emitted shader
                // is a single self-contained source string the compiler
                // can hand straight to shaderc.
                const char *kNoisePreamble = R"GLSL(
// Hash + permutation table for Perlin/simplex (Stefan Gustavson, public
// domain). Compact mod289 / permute formulation — no lookup textures.
vec3 vop_mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 vop_mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 vop_permute(vec4 x) { return vop_mod289(((x*34.0)+1.0)*x); }
vec4 vop_taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
vec3 vop_fade(vec3 t) { return t*t*t*(t*(t*6.0-15.0)+10.0); }

// Seed offset (matches CPU `seedShift`).
vec3 vop_seed_shift(vec3 p, int seed) {
    float s = float(seed);
    return vec3(p.x + s * 17.13, p.y + s * 31.71, p.z + s * 53.91);
}

// Classic 3D Perlin noise. Output in roughly [-1, 1].
float vop_perlin(vec3 P) {
    vec3 Pi0 = floor(P);
    vec3 Pi1 = Pi0 + vec3(1.0);
    Pi0 = vop_mod289(Pi0);
    Pi1 = vop_mod289(Pi1);
    vec3 Pf0 = fract(P);
    vec3 Pf1 = Pf0 - vec3(1.0);
    vec4 ix = vec4(Pi0.x, Pi1.x, Pi0.x, Pi1.x);
    vec4 iy = vec4(Pi0.yy, Pi1.yy);
    vec4 iz0 = vec4(Pi0.z);
    vec4 iz1 = vec4(Pi1.z);

    vec4 ixy = vop_permute(vop_permute(ix) + iy);
    vec4 ixy0 = vop_permute(ixy + iz0);
    vec4 ixy1 = vop_permute(ixy + iz1);

    vec4 gx0 = ixy0 * (1.0 / 7.0);
    vec4 gy0 = fract(floor(gx0) * (1.0 / 7.0)) - 0.5;
    gx0 = fract(gx0);
    vec4 gz0 = vec4(0.5) - abs(gx0) - abs(gy0);
    vec4 sz0 = step(gz0, vec4(0.0));
    gx0 -= sz0 * (step(0.0, gx0) - 0.5);
    gy0 -= sz0 * (step(0.0, gy0) - 0.5);

    vec4 gx1 = ixy1 * (1.0 / 7.0);
    vec4 gy1 = fract(floor(gx1) * (1.0 / 7.0)) - 0.5;
    gx1 = fract(gx1);
    vec4 gz1 = vec4(0.5) - abs(gx1) - abs(gy1);
    vec4 sz1 = step(gz1, vec4(0.0));
    gx1 -= sz1 * (step(0.0, gx1) - 0.5);
    gy1 -= sz1 * (step(0.0, gy1) - 0.5);

    vec3 g000 = vec3(gx0.x, gy0.x, gz0.x);
    vec3 g100 = vec3(gx0.y, gy0.y, gz0.y);
    vec3 g010 = vec3(gx0.z, gy0.z, gz0.z);
    vec3 g110 = vec3(gx0.w, gy0.w, gz0.w);
    vec3 g001 = vec3(gx1.x, gy1.x, gz1.x);
    vec3 g101 = vec3(gx1.y, gy1.y, gz1.y);
    vec3 g011 = vec3(gx1.z, gy1.z, gz1.z);
    vec3 g111 = vec3(gx1.w, gy1.w, gz1.w);

    vec4 norm0 = vop_taylorInvSqrt(vec4(dot(g000, g000), dot(g010, g010), dot(g100, g100), dot(g110, g110)));
    g000 *= norm0.x; g010 *= norm0.y; g100 *= norm0.z; g110 *= norm0.w;
    vec4 norm1 = vop_taylorInvSqrt(vec4(dot(g001, g001), dot(g011, g011), dot(g101, g101), dot(g111, g111)));
    g001 *= norm1.x; g011 *= norm1.y; g101 *= norm1.z; g111 *= norm1.w;

    float n000 = dot(g000, Pf0);
    float n100 = dot(g100, vec3(Pf1.x, Pf0.yz));
    float n010 = dot(g010, vec3(Pf0.x, Pf1.y, Pf0.z));
    float n110 = dot(g110, vec3(Pf1.xy, Pf0.z));
    float n001 = dot(g001, vec3(Pf0.xy, Pf1.z));
    float n101 = dot(g101, vec3(Pf1.x, Pf0.y, Pf1.z));
    float n011 = dot(g011, vec3(Pf0.x, Pf1.yz));
    float n111 = dot(g111, Pf1);

    vec3 fade_xyz = vop_fade(Pf0);
    vec4 n_z = mix(vec4(n000, n100, n010, n110), vec4(n001, n101, n011, n111), fade_xyz.z);
    vec2 n_yz = mix(n_z.xy, n_z.zw, fade_xyz.y);
    float n_xyz = mix(n_yz.x, n_yz.y, fade_xyz.x);
    return 2.2 * n_xyz;
}

// fBm — sum of N octaves of Perlin, each at double frequency and
// half amplitude (configurable via lacunarity + gain). The single
// biggest noise quality-of-life upgrade. Octave count is clamped to
// [1, 10] so the loop has a static upper bound that the compiler
// can unroll/predicate.
float vop_noise_fbm(vec3 sp, int octaves, float lac, float gain) {
    int n = clamp(octaves, 1, 10);
    float amp = 1.0;
    float sum = 0.0;
    for (int o = 0; o < 10; ++o) {
        if (o >= n) break;
        sum += vop_perlin(sp) * amp;
        sp *= lac;
        amp *= gain;
    }
    return sum;
}

// Turbulence — fBm of |perlin|. Always non-negative; classic
// billowy / marbled look (clouds, smoke, marble veining).
float vop_noise_turbulence(vec3 sp, int octaves, float lac, float gain) {
    int n = clamp(octaves, 1, 10);
    float amp = 1.0;
    float sum = 0.0;
    for (int o = 0; o < 10; ++o) {
        if (o >= n) break;
        sum += abs(vop_perlin(sp)) * amp;
        sp *= lac;
        amp *= gain;
    }
    return sum;
}

// Ridged multifractal — (1 - |perlin|)^2 octaves. Sharp positive
// ridges with wide valleys. The "mountain peaks" / "tree-bark" look.
float vop_noise_ridged(vec3 sp, int octaves, float lac, float gain) {
    int n = clamp(octaves, 1, 10);
    float amp = 1.0;
    float sum = 0.0;
    for (int o = 0; o < 10; ++o) {
        if (o >= n) break;
        float r = 1.0 - abs(vop_perlin(sp));
        sum += r * r * amp;
        sp *= lac;
        amp *= gain;
    }
    return sum;
}

// Curl of three offset Perlin potentials — divergence-free vector
// noise. Particles advected by this field swirl without diverging,
// giving the canonical "fluid-like" particle motion. Central
// differences on `eps` approximate the analytic curl; eps is
// clamped on the host so we don't have to worry about 0 here.
vec3 vop_noise_curl(vec3 base, int seed, float eps) {
    vec3 dx = vec3(eps, 0.0, 0.0);
    vec3 dy = vec3(0.0, eps, 0.0);
    vec3 dz = vec3(0.0, 0.0, eps);
    float two_eps = 2.0 * eps;
    // Three independent scalar potentials, offset by 41/83 in seed
    // so adjacent integer seeds give decorrelated fields. Matches
    // the CPU evaluator exactly.
    float dPzdy = (vop_perlin(vop_seed_shift(base + dy, seed + 83))
                 - vop_perlin(vop_seed_shift(base - dy, seed + 83))) / two_eps;
    float dPydz = (vop_perlin(vop_seed_shift(base + dz, seed + 41))
                 - vop_perlin(vop_seed_shift(base - dz, seed + 41))) / two_eps;
    float dPxdz = (vop_perlin(vop_seed_shift(base + dz, seed))
                 - vop_perlin(vop_seed_shift(base - dz, seed))) / two_eps;
    float dPzdx = (vop_perlin(vop_seed_shift(base + dx, seed + 83))
                 - vop_perlin(vop_seed_shift(base - dx, seed + 83))) / two_eps;
    float dPydx = (vop_perlin(vop_seed_shift(base + dx, seed + 41))
                 - vop_perlin(vop_seed_shift(base - dx, seed + 41))) / two_eps;
    float dPxdy = (vop_perlin(vop_seed_shift(base + dy, seed))
                 - vop_perlin(vop_seed_shift(base - dy, seed))) / two_eps;
    return vec3(dPzdy - dPydz, dPxdz - dPzdx, dPydx - dPxdy);
}

// Worley (cellular) F1 noise. Returns the distance to the nearest
// jittered feature point in the 3-cell neighbourhood. CPU mirror lives
// in src/vops/nodes/noise_vops.cpp::worleyF1; the hash constants here
// match hash3i so the same (P, seed) produces the same value on either
// path.
uint vop_hash3i(ivec3 c, int seed) {
    uint x = uint(c.x) * 2654435761u
           ^ uint(c.y) * 2246822519u
           ^ uint(c.z) *  374761393u
           ^ uint(seed) * 3266489917u;
    x ^= x >> 13; x *= 0x85ebca6bu;
    x ^= x >> 16; x *= 0xc2b2ae35u;
    x ^= x >> 13;
    return x;
}
vec3 vop_worley_feature(ivec3 c, int seed) {
    // Three independent [0,1) floats out of three hashes — must match
    // hashedFeaturePoint on the CPU (seed+0, seed+1013, seed+1031).
    uint hx = vop_hash3i(c, seed);
    uint hy = vop_hash3i(c, seed + 1013);
    uint hz = vop_hash3i(c, seed + 1031);
    return vec3(float(hx & 0x00ffffffu) / float(0x01000000u),
                float(hy & 0x00ffffffu) / float(0x01000000u),
                float(hz & 0x00ffffffu) / float(0x01000000u));
}
float vop_worley_f1(vec3 p, int seed) {
    ivec3 ip = ivec3(floor(p));
    float bestSq = 1e30;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        ivec3 c = ip + ivec3(dx, dy, dz);
        vec3 jitter = vop_worley_feature(c, seed);
        vec3 fp = vec3(c) + jitter;
        vec3 d  = fp - p;
        bestSq  = min(bestSq, dot(d, d));
    }
    return sqrt(bestSq);
}

// Simplex noise (3D). Same source — public-domain reference. Faster than
// Perlin in 3D+ and with less directional bias.
float vop_simplex(vec3 v) {
    const vec2 C = vec2(1.0/6.0, 1.0/3.0);
    const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
    vec3 i  = floor(v + dot(v, C.yyy));
    vec3 x0 = v - i + dot(i, C.xxx);
    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);
    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - D.yyy;
    i = vop_mod289(i);
    vec4 p = vop_permute(vop_permute(vop_permute(
              i.z + vec4(0.0, i1.z, i2.z, 1.0))
            + i.y + vec4(0.0, i1.y, i2.y, 1.0))
            + i.x + vec4(0.0, i1.x, i2.x, 1.0));
    float n_ = 0.142857142857;
    vec3 ns = n_ * D.wyz - D.xzx;
    vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
    vec4 x_ = floor(j * ns.z);
    vec4 y_ = floor(j - 7.0 * x_);
    vec4 x = x_ * ns.x + ns.yyyy;
    vec4 y = y_ * ns.x + ns.yyyy;
    vec4 h = 1.0 - abs(x) - abs(y);
    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);
    vec4 s0 = floor(b0)*2.0 + 1.0;
    vec4 s1 = floor(b1)*2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));
    vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;
    vec3 p0 = vec3(a0.xy, h.x);
    vec3 p1 = vec3(a0.zw, h.y);
    vec3 p2 = vec3(a1.xy, h.z);
    vec3 p3 = vec3(a1.zw, h.w);
    vec4 norm = vop_taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
    vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}
)GLSL";

                // Emit the SSBO + push-constant declarations and the main()
                // wrapper around the per-node body.
                std::string assembleShader(const EmitState &state)
                {
                    std::ostringstream out;
                    out << "#version 460\n";
                    out << "layout(local_size_x = " << state.result.localSizeX << ") in;\n\n";

                    // Attribute SSBOs — one per touched attribute. std430
                    // requires vec3 arrays to be 16-byte-strided, so we
                    // store vec3 attrs as `vec4 data[]` (.xyz holds the
                    // value, .w is unused padding). Float attrs pack
                    // naturally at 4 bytes/element. The dispatcher uploads
                    // CPU-side data with the matching layout.
                    for (const auto &a : state.result.attrs)
                    {
                        const char *storage = (a.type == GpuType::Vec3) ? "vec4" : gpuTypeStr(a.type);
                        out << "layout(std430, binding = " << a.binding << ") buffer Attr_"
                            << a.name << " { " << storage << " data[]; } attr_"
                            << a.name << ";\n";
                    }

                    // Param SSBO. Always declared — even when empty — so
                    // the kernel body's `params.data[...]` references stay
                    // valid for graphs with zero parameters (rare in
                    // practice; pad to at least one slot if needed).
                    out << "layout(std430, binding = " << state.result.paramsBinding
                        << ") readonly buffer Params { vec4 data[]; } params;\n";

                    // Push constant: at least pointCount, so the kernel can
                    // bail past the end when the dispatch is rounded up.
                    out << "layout(push_constant) uniform PC { uint pointCount; } pc;\n\n";

                    out << kNoisePreamble << "\n";

                    out << "void main() {\n";
                    out << "    uint pi = gl_GlobalInvocationID.x;\n";
                    out << "    if (pi >= pc.pointCount) return;\n";
                    out << state.body.str();
                    out << "}\n";
                    return out.str();
                }
            } // anon

            EmitResult emitGlsl(const VopGraph &graph)
            {
                EmitState state;
                state.result.localSizeX = 64;
                graph.compile();

                const auto &order = graph.topoOrder();
                if (order.size() > kMaxNodes)
                {
                    // Hard ceiling so a runaway graph doesn't OOM the
                    // compiler. The caller can fall back to CPU eval.
                    state.result.unsupported.push_back("__too_many_nodes");
                    state.result.glsl = "";
                    return state.result;
                }

                // The geo_input emitter strips its own unused output
                // ports per-port (see emitGeoInput's outputHasConsumer
                // check) — no top-level node dead-strip needed.
                for (size_t uid : order)
                {
                    const VopNode *node = graph.findNode(uid);
                    if (!node) continue;
                    if (!emitNode(state, graph, *node))
                    {
                        state.body << "    // unsupported: " << node->kind() << "\n";
                        state.result.unsupported.push_back(node->kind());
                    }
                }

                // Reserve the params binding AFTER all attrs got registered.
                state.result.paramsBinding = static_cast<uint32_t>(state.result.attrs.size());
                // Pad param count to at least 1 — std430 SSBOs with a
                // runtime-length array must back at least one element.
                if (state.result.paramSlotCount == 0) state.result.paramSlotCount = 1;

                state.result.glsl = assembleShader(state);
                return state.result;
            }

            uint64_t hashGlsl(const std::string &source)
            {
                constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
                constexpr uint64_t kFnvPrime  = 0x100000001b3ULL;
                uint64_t h = kFnvOffset;
                for (unsigned char c : source)
                {
                    h ^= c;
                    h *= kFnvPrime;
                }
                return h;
            }
        }
    }
}
