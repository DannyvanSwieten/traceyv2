#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace tracey
{
    namespace vops
    {
        namespace
        {
            // Resolve a Vec3 from whatever flavour of Value the upstream
            // wire happens to carry. Reused by every noise node, so
            // disconnected `p` ports default to the origin.
            Vec3 readPositionInput(EvalContext &ctx, size_t nodeUid, size_t portIdx)
            {
                auto in = ctx.graph->readInput(ctx, nodeUid, portIdx).value_or(Value{Vec3(0.0f)});
                if (auto *v = std::get_if<Vec3>(&in)) return *v;
                if (auto *f = std::get_if<float>(&in)) return Vec3(*f);
                if (auto *i = std::get_if<int>(&in)) return Vec3(static_cast<float>(*i));
                return Vec3(0.0f);
            }

            // Shift a sample point into a per-seed slice of the noise's
            // continuous domain. Adjacent integer seeds give meaningfully
            // different fields without crowding (the offsets are large
            // irrational-ish strides, not powers of two).
            glm::vec3 seedShift(const glm::vec3 &p, int seed)
            {
                const float so = static_cast<float>(seed);
                return glm::vec3(p.x + so * 17.13f,
                                 p.y + so * 31.71f,
                                 p.z + so * 53.91f);
            }

            // 32-bit integer hash used by the cellular noise (Worley) below
            // for feature-point placement. Stable across seeds: the hash
            // mixes (x, y, z, seed) into a single uint that's then split
            // into three [0,1) coordinates by dividing successive 10-bit
            // slices. Cheap; quality is plenty for visible jittered cells.
            uint32_t hash3i(int x, int y, int z, int seed)
            {
                uint32_t h = static_cast<uint32_t>(x) * 2654435761u
                           ^ static_cast<uint32_t>(y) * 2246822519u
                           ^ static_cast<uint32_t>(z) *  374761393u
                           ^ static_cast<uint32_t>(seed) * 3266489917u;
                h ^= h >> 13; h *= 0x85ebca6bu;
                h ^= h >> 16; h *= 0xc2b2ae35u;
                h ^= h >> 13;
                return h;
            }
            // Three independent [0,1) floats packed out of one hash.
            // Reasonable distribution for jitter; not crypto.
            glm::vec3 hashedFeaturePoint(int x, int y, int z, int seed)
            {
                const uint32_t hx = hash3i(x, y, z, seed);
                const uint32_t hy = hash3i(x, y, z, seed + 1013);
                const uint32_t hz = hash3i(x, y, z, seed + 1031);
                return glm::vec3(
                    static_cast<float>(hx & 0x00ffffffu) / static_cast<float>(0x01000000u),
                    static_cast<float>(hy & 0x00ffffffu) / static_cast<float>(0x01000000u),
                    static_cast<float>(hz & 0x00ffffffu) / static_cast<float>(0x01000000u));
            }

            // Worley (cellular) F1 noise. Returns the distance to the
            // nearest jittered feature point, expressed as a [0,1)-ish
            // value (the cell's edge length is 1.0 in the sampled domain).
            // Hexagons / rocks / cell patterns are the canonical use.
            float worleyF1(glm::vec3 p, int seed)
            {
                const int ix = static_cast<int>(std::floor(p.x));
                const int iy = static_cast<int>(std::floor(p.y));
                const int iz = static_cast<int>(std::floor(p.z));
                float bestSq = 1e30f;
                for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                    glm::vec3 jitter = hashedFeaturePoint(cx, cy, cz, seed);
                    glm::vec3 fp = glm::vec3(cx, cy, cz) + jitter;
                    glm::vec3 d = fp - p;
                    const float sq = glm::dot(d, d);
                    if (sq < bestSq) bestSq = sq;
                }
                return std::sqrt(bestSq);
            }
        }

        // ── noise_perlin ─────────────────────────────────────────────────────
        // 3D Perlin noise at the input position. Output is signed in roughly
        // [-amplitude, +amplitude] (glm::perlin tops out at about ±0.7).
        class NoisePerlinVop : public VopNode
        {
        public:
            explicit NoisePerlinVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_perlin"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const int   seed = paramInt("seed", 0);
                const glm::vec3 sp = seedShift(glm::vec3(p.x, p.y, p.z) * freq, seed);
                ctx.graph->writeOutput(ctx, uid(), 0, glm::perlin(sp) * amp);
            }
        };

        // ── noise_simplex ────────────────────────────────────────────────────
        // 3D Simplex noise — smoother than Perlin, less directional bias,
        // generally faster in higher dimensions. Same signature as Perlin
        // so the user can swap one for the other without rewiring.
        class NoiseSimplexVop : public VopNode
        {
        public:
            explicit NoiseSimplexVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_simplex"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const int   seed = paramInt("seed", 0);
                const glm::vec3 sp = seedShift(glm::vec3(p.x, p.y, p.z) * freq, seed);
                ctx.graph->writeOutput(ctx, uid(), 0, glm::simplex(sp) * amp);
            }
        };

        // ── noise_worley ─────────────────────────────────────────────────────
        // Cellular (Worley) F1 noise — output is the distance to the
        // nearest jittered feature point in the sampled lattice. Reads as
        // hexagonal cells / rocks / scales. Output is in [0, ~1] (always
        // non-negative) so `amplitude` scales linearly without a sign flip.
        class NoiseWorleyVop : public VopNode
        {
        public:
            explicit NoiseWorleyVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_worley"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const int   seed = paramInt("seed", 0);
                const glm::vec3 sp(p.x * freq, p.y * freq, p.z * freq);
                ctx.graph->writeOutput(ctx, uid(), 0, worleyF1(sp, seed) * amp);
            }
        };

        // ── noise_fbm ────────────────────────────────────────────────────────
        // Fractal Brownian Motion: sum N octaves of Perlin, each octave
        // doubling frequency (× lacunarity) and halving amplitude (× gain).
        // The single biggest noise quality-of-life upgrade — turns Perlin's
        // "blobs" into multi-scale detail (terrain, clouds, marble, etc.).
        // Output is roughly signed; magnitude depends on gain & octaves.
        class NoiseFbmVop : public VopNode
        {
        public:
            explicit NoiseFbmVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("octaves", 5));
                declareParam(Parameter::makeFloat("lacunarity", 2.0f));
                declareParam(Parameter::makeFloat("gain", 0.5f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_fbm"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq0 = paramFloat("frequency", 1.0f);
                const float amp0  = paramFloat("amplitude", 1.0f);
                // Cap octave count generously to keep accidental large
                // values from grinding the cook to a halt.
                const int octaves = std::max(1, std::min(paramInt("octaves", 5), 10));
                const float lac = paramFloat("lacunarity", 2.0f);
                const float gain = paramFloat("gain", 0.5f);
                const int seed = paramInt("seed", 0);

                glm::vec3 sp = seedShift(glm::vec3(p.x, p.y, p.z) * freq0, seed);
                float amp = 1.0f;
                float sum = 0.0f;
                for (int o = 0; o < octaves; ++o)
                {
                    sum += glm::perlin(sp) * amp;
                    sp *= lac;
                    amp *= gain;
                }
                ctx.graph->writeOutput(ctx, uid(), 0, sum * amp0);
            }
        };

        // ── noise_turbulence ─────────────────────────────────────────────────
        // Turbulence = fBm of |perlin|. Output is non-negative and has the
        // characteristic billowy / marbled look (think clouds, smoke,
        // marble veining). Same octaves/lacunarity/gain controls as fBm.
        class NoiseTurbulenceVop : public VopNode
        {
        public:
            explicit NoiseTurbulenceVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("octaves", 5));
                declareParam(Parameter::makeFloat("lacunarity", 2.0f));
                declareParam(Parameter::makeFloat("gain", 0.5f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_turbulence"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq0 = paramFloat("frequency", 1.0f);
                const float amp0  = paramFloat("amplitude", 1.0f);
                const int octaves = std::max(1, std::min(paramInt("octaves", 5), 10));
                const float lac = paramFloat("lacunarity", 2.0f);
                const float gain = paramFloat("gain", 0.5f);
                const int seed = paramInt("seed", 0);

                glm::vec3 sp = seedShift(glm::vec3(p.x, p.y, p.z) * freq0, seed);
                float amp = 1.0f;
                float sum = 0.0f;
                for (int o = 0; o < octaves; ++o)
                {
                    sum += std::abs(glm::perlin(sp)) * amp;
                    sp *= lac;
                    amp *= gain;
                }
                ctx.graph->writeOutput(ctx, uid(), 0, sum * amp0);
            }
        };

        // ── noise_ridged ─────────────────────────────────────────────────────
        // Ridged multifractal — folds |perlin| into 1 - |perlin| (and
        // squares the result) so the noise has sharp positive ridges with
        // wide valleys. The classic "mountain peaks" / "tree-bark" look.
        class NoiseRidgedVop : public VopNode
        {
        public:
            explicit NoiseRidgedVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("octaves", 5));
                declareParam(Parameter::makeFloat("lacunarity", 2.0f));
                declareParam(Parameter::makeFloat("gain", 0.5f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_ridged"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq0 = paramFloat("frequency", 1.0f);
                const float amp0  = paramFloat("amplitude", 1.0f);
                const int octaves = std::max(1, std::min(paramInt("octaves", 5), 10));
                const float lac = paramFloat("lacunarity", 2.0f);
                const float gain = paramFloat("gain", 0.5f);
                const int seed = paramInt("seed", 0);

                glm::vec3 sp = seedShift(glm::vec3(p.x, p.y, p.z) * freq0, seed);
                float amp = 1.0f;
                float sum = 0.0f;
                for (int o = 0; o < octaves; ++o)
                {
                    float r = 1.0f - std::abs(glm::perlin(sp));
                    r = r * r;  // sharpen — classic Musgrave variant
                    sum += r * amp;
                    sp *= lac;
                    amp *= gain;
                }
                ctx.graph->writeOutput(ctx, uid(), 0, sum * amp0);
            }
        };

        // ── noise_vec3 ───────────────────────────────────────────────────────
        // Three independent Perlin samples → Vec3. The three channels are
        // pulled from offset slices of the same continuous field so the
        // result is decorrelated. Useful for vector displacement, jitter
        // direction, RGB noise, etc.
        class NoiseVec3Vop : public VopNode
        {
        public:
            explicit NoiseVec3Vop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const int   seed = paramInt("seed", 0);
                const glm::vec3 base(p.x * freq, p.y * freq, p.z * freq);
                Vec3 r(glm::perlin(seedShift(base, seed)) * amp,
                       glm::perlin(seedShift(base, seed + 41)) * amp,
                       glm::perlin(seedShift(base, seed + 83)) * amp);
                ctx.graph->writeOutput(ctx, uid(), 0, r);
            }
        };

        // ── noise_curl ───────────────────────────────────────────────────────
        // Divergence-free vector noise: curl of three offset Perlin
        // potentials. Useful for fluid-looking advection — points pushed
        // along this field swirl without diverging or converging. Output
        // is a Vec3 in roughly [-amplitude, +amplitude] per component.
        //
        // Implementation uses analytic-style central differences on three
        // perlin samples — works well enough for visual use; not a
        // rigorous reproduction of Bridson's original construction.
        class NoiseCurlVop : public VopNode
        {
        public:
            explicit NoiseCurlVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeFloat("eps", 0.001f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_curl"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p = readPositionInput(ctx, uid(), 0);
                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const float eps  = std::max(1e-5f, paramFloat("eps", 0.001f));
                const int   seed = paramInt("seed", 0);
                const glm::vec3 base(p.x * freq, p.y * freq, p.z * freq);

                // Three independent scalar potentials, one per axis pair.
                // curl = (∂Pz/∂y − ∂Py/∂z, ∂Px/∂z − ∂Pz/∂x, ∂Py/∂x − ∂Px/∂y)
                auto Px = [&](glm::vec3 q) { return glm::perlin(seedShift(q, seed)); };
                auto Py = [&](glm::vec3 q) { return glm::perlin(seedShift(q, seed + 41)); };
                auto Pz = [&](glm::vec3 q) { return glm::perlin(seedShift(q, seed + 83)); };

                const glm::vec3 dx(eps, 0, 0), dy(0, eps, 0), dz(0, 0, eps);
                const float dPzdy = (Pz(base + dy) - Pz(base - dy)) / (2.0f * eps);
                const float dPydz = (Py(base + dz) - Py(base - dz)) / (2.0f * eps);
                const float dPxdz = (Px(base + dz) - Px(base - dz)) / (2.0f * eps);
                const float dPzdx = (Pz(base + dx) - Pz(base - dx)) / (2.0f * eps);
                const float dPydx = (Py(base + dx) - Py(base - dx)) / (2.0f * eps);
                const float dPxdy = (Px(base + dy) - Px(base - dy)) / (2.0f * eps);

                Vec3 curl(dPzdy - dPydz, dPxdz - dPzdx, dPydx - dPxdy);
                ctx.graph->writeOutput(ctx, uid(), 0, curl * amp);
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

        void registerNoiseVops()
        {
            auto &reg = VopRegistry::instance();

            // Catalog params for the octaved variants are identical — pull
            // them into a constant to keep the registrations readable.
            // Ranges turn the inspector inputs into sliders. seed has no
            // natural range (any int is valid) so stays a plain number
            // input. Aggregate-init order: {name,type,default,min,max,step}.
            const std::vector<ParamSpec> kOctavedParams = {
                {"frequency",  ParamType::Float, "1.0", 0.0,  8.0,  0.01},
                {"amplitude",  ParamType::Float, "1.0", 0.0,  4.0,  0.01},
                {"octaves",    ParamType::Int,   "5",   1.0,  10.0, 1.0},
                {"lacunarity", ParamType::Float, "2.0", 1.0,  4.0,  0.01},
                {"gain",       ParamType::Float, "0.5", 0.0,  1.0,  0.01},
                {"seed",       ParamType::Int,   "0"},
            };
            const std::vector<ParamSpec> kBasicParams = {
                {"frequency", ParamType::Float, "1.0", 0.0, 8.0, 0.01},
                {"amplitude", ParamType::Float, "1.0", 0.0, 4.0, 0.01},
                {"seed",      ParamType::Int,   "0"},
            };

            reg.registerType(
                {"noise_perlin", "Perlin Noise", "Noise",
                 {{"p"}}, {{"out"}}, kBasicParams},
                makeFactory<NoisePerlinVop>());
            reg.registerType(
                {"noise_simplex", "Simplex Noise", "Noise",
                 {{"p"}}, {{"out"}}, kBasicParams},
                makeFactory<NoiseSimplexVop>());
            reg.registerType(
                {"noise_worley", "Worley (Cellular)", "Noise",
                 {{"p"}}, {{"out"}}, kBasicParams},
                makeFactory<NoiseWorleyVop>());
            reg.registerType(
                {"noise_fbm", "Fractal (fBm)", "Noise",
                 {{"p"}}, {{"out"}}, kOctavedParams},
                makeFactory<NoiseFbmVop>());
            reg.registerType(
                {"noise_turbulence", "Turbulence", "Noise",
                 {{"p"}}, {{"out"}}, kOctavedParams},
                makeFactory<NoiseTurbulenceVop>());
            reg.registerType(
                {"noise_ridged", "Ridged Multifractal", "Noise",
                 {{"p"}}, {{"out"}}, kOctavedParams},
                makeFactory<NoiseRidgedVop>());
            reg.registerType(
                {"noise_vec3", "Noise (Vec3)", "Noise",
                 {{"p"}}, {{"out"}}, kBasicParams},
                makeFactory<NoiseVec3Vop>());
            reg.registerType(
                {"noise_curl", "Curl Noise", "Noise",
                 {{"p"}}, {{"out"}},
                 {{"frequency", ParamType::Float, "1.0", 0.0, 8.0, 0.01},
                  {"amplitude", ParamType::Float, "1.0", 0.0, 4.0, 0.01},
                  // eps controls the central-difference offset for the
                  // curl gradient — too coarse smears the field, too fine
                  // hits float precision noise. The reasonable band is
                  // narrow so keep it as a plain number input.
                  {"eps",       ParamType::Float, "0.001"},
                  {"seed",      ParamType::Int,   "0"}}},
                makeFactory<NoiseCurlVop>());
        }
    }
}
