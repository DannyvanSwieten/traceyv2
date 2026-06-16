// Generator SOP: MoText — a text string rasterized to glyph outlines and
// extruded into a 3D triangle mesh (front face + back face + side walls).
//
// Pipeline per glyph: stb_truetype outline -> flatten quadratic/cubic beziers
// to closed contours -> classify outer vs hole by containment -> earcut
// triangulate each {outer + holes} group for the front face -> extrude.
//
// A "curve" elsewhere in MoGraph is a point cloud; MoText instead emits a real
// triangle mesh (vertex-class N for crisp edges), so it renders directly and
// can also feed copy_to_points for per-letter MoGraph.

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <earcut.hpp>

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            using Pt2 = std::array<float, 2>;
            using Contour = std::vector<Pt2>;

            // ── UTF-8 decode (with "\n" escape -> newline since the param is
            //    single-line) ──
            std::vector<uint32_t> decodeText(const std::string &raw)
            {
                // Replace the two-char escape backslash-n with a real newline.
                std::string s;
                s.reserve(raw.size());
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n')
                    {
                        s.push_back('\n');
                        ++i;
                    }
                    else
                        s.push_back(raw[i]);
                }

                std::vector<uint32_t> out;
                for (size_t i = 0; i < s.size();)
                {
                    const unsigned char c = static_cast<unsigned char>(s[i]);
                    uint32_t cp = 0;
                    int extra = 0;
                    if (c < 0x80) { cp = c; extra = 0; }
                    else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
                    else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
                    else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
                    else { ++i; continue; } // invalid lead byte
                    if (i + extra >= s.size()) break;
                    for (int k = 0; k < extra; ++k)
                        cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1 + k]) & 0x3F);
                    i += extra + 1;
                    out.push_back(cp);
                }
                return out;
            }

            float signedArea(const Contour &c)
            {
                float a = 0.0f;
                const size_t n = c.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const Pt2 &p = c[i];
                    const Pt2 &q = c[(i + 1) % n];
                    a += p[0] * q[1] - q[0] * p[1];
                }
                return 0.5f * a;
            }

            bool pointInPolygon(const Pt2 &pt, const Contour &poly)
            {
                bool inside = false;
                const size_t n = poly.size();
                for (size_t i = 0, j = n - 1; i < n; j = i++)
                {
                    const float xi = poly[i][0], yi = poly[i][1];
                    const float xj = poly[j][0], yj = poly[j][1];
                    if (((yi > pt[1]) != (yj > pt[1])) &&
                        (pt[0] < (xj - xi) * (pt[1] - yi) / (yj - yi + 1e-20f) + xi))
                        inside = !inside;
                }
                return inside;
            }

            // Flatten a quadratic bezier p0->c->p1 into line segments.
            void flattenQuadratic(const Pt2 &p0, const Pt2 &c, const Pt2 &p1,
                                  float tol, Contour &out)
            {
                // Subdivision count from control-point deviation; clamped.
                const float dx = c[0] - 0.5f * (p0[0] + p1[0]);
                const float dy = c[1] - 0.5f * (p0[1] + p1[1]);
                const float dev = std::sqrt(dx * dx + dy * dy);
                int n = std::max(2, static_cast<int>(std::ceil(std::sqrt(dev / std::max(tol, 1e-4f)))));
                n = std::min(n, 32);
                for (int i = 1; i <= n; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(n);
                    const float u = 1.0f - t;
                    out.push_back({u * u * p0[0] + 2 * u * t * c[0] + t * t * p1[0],
                                   u * u * p0[1] + 2 * u * t * c[1] + t * t * p1[1]});
                }
            }

            void flattenCubic(const Pt2 &p0, const Pt2 &c0, const Pt2 &c1, const Pt2 &p1,
                              float tol, Contour &out)
            {
                const float d0 = std::hypot(c0[0] - p0[0], c0[1] - p0[1]);
                const float d1 = std::hypot(c1[0] - c0[0], c1[1] - c0[1]);
                const float d2 = std::hypot(p1[0] - c1[0], p1[1] - c1[1]);
                int n = std::max(2, static_cast<int>(std::ceil(std::sqrt((d0 + d1 + d2) / std::max(tol, 1e-4f)))));
                n = std::min(n, 48);
                for (int i = 1; i <= n; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(n);
                    const float u = 1.0f - t;
                    const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                    out.push_back({w0 * p0[0] + w1 * c0[0] + w2 * c1[0] + w3 * p1[0],
                                   w0 * p0[1] + w1 * c0[1] + w2 * c1[1] + w3 * p1[1]});
                }
            }

            // Extract a glyph's contours (in em-scaled glyph-local coords).
            std::vector<Contour> glyphContours(const stbtt_fontinfo &font, int cp,
                                               float scale, float tol)
            {
                stbtt_vertex *verts = nullptr;
                const int nv = stbtt_GetCodepointShape(&font, cp, &verts);
                std::vector<Contour> contours;
                Contour cur;
                Pt2 pen{0.0f, 0.0f};
                auto sv = [scale](short v) { return static_cast<float>(v) * scale; };
                for (int i = 0; i < nv; ++i)
                {
                    const stbtt_vertex &v = verts[i];
                    const Pt2 to{sv(v.x), sv(v.y)};
                    switch (v.type)
                    {
                    case STBTT_vmove:
                        if (cur.size() >= 3) contours.push_back(cur);
                        cur.clear();
                        cur.push_back(to);
                        pen = to;
                        break;
                    case STBTT_vline:
                        cur.push_back(to);
                        pen = to;
                        break;
                    case STBTT_vcurve:
                        flattenQuadratic(pen, {sv(v.cx), sv(v.cy)}, to, tol, cur);
                        pen = to;
                        break;
                    case STBTT_vcubic:
                        flattenCubic(pen, {sv(v.cx), sv(v.cy)}, {sv(v.cx1), sv(v.cy1)}, to, tol, cur);
                        pen = to;
                        break;
                    default:
                        break;
                    }
                }
                if (cur.size() >= 3) contours.push_back(cur);
                if (verts) stbtt_FreeShape(&font, verts);
                // Drop near-degenerate contours.
                contours.erase(std::remove_if(contours.begin(), contours.end(),
                                              [](const Contour &c) {
                                                  return c.size() < 3 || std::fabs(signedArea(c)) < 1e-6f;
                                              }),
                               contours.end());
                return contours;
            }

            class MoTextSop : public SopNode
            {
            public:
                explicit MoTextSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("font_file", "/System/Library/Fonts/Helvetica.ttc"));
                    declareParam(Parameter::makeString("text", "Text"));
                    declareParam(Parameter::makeFloat("size", 1.0f));
                    declareParam(Parameter::makeFloat("depth", 0.2f));
                    declareParam(Parameter::makeFloat("tolerance", 0.002f));
                    declareParam(Parameter::makeString("align", "center"));
                }

                std::string kind() const override { return "mo_text"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const std::string fontPath = paramString("font_file", "");
                    const std::string text = paramString("text", "");
                    const float size = paramFloat("size", 1.0f);
                    const float depth = paramFloat("depth", 0.2f);
                    const float tol = std::max(1e-4f, paramFloat("tolerance", 0.002f));
                    const std::string align = paramString("align", "center");
                    if (text.empty() || size <= 0.0f) return {};

                    // Load the font file.
                    std::ifstream f(fontPath, std::ios::binary);
                    if (!f) return {};
                    std::vector<unsigned char> fontData((std::istreambuf_iterator<char>(f)),
                                                        std::istreambuf_iterator<char>());
                    if (fontData.empty()) return {};
                    stbtt_fontinfo font;
                    if (!stbtt_InitFont(&font, fontData.data(),
                                        stbtt_GetFontOffsetForIndex(fontData.data(), 0)))
                        return {};

                    const float scale = stbtt_ScaleForMappingEmToPixels(&font, size);
                    int ascentI, descentI, lineGapI;
                    stbtt_GetFontVMetrics(&font, &ascentI, &descentI, &lineGapI);
                    const float lineAdvance = (ascentI - descentI + lineGapI) * scale;

                    const auto codepoints = decodeText(text);

                    // Split into lines and pre-measure widths for centering.
                    std::vector<std::vector<uint32_t>> lines(1);
                    for (uint32_t cp : codepoints)
                    {
                        if (cp == '\n') lines.emplace_back();
                        else lines.back().push_back(cp);
                    }
                    auto lineWidth = [&](const std::vector<uint32_t> &ln) {
                        float w = 0.0f;
                        for (size_t i = 0; i < ln.size(); ++i)
                        {
                            int adv, lsb;
                            stbtt_GetCodepointHMetrics(&font, static_cast<int>(ln[i]), &adv, &lsb);
                            if (i > 0)
                                w += stbtt_GetCodepointKernAdvance(
                                         &font, static_cast<int>(ln[i - 1]),
                                         static_cast<int>(ln[i])) * scale;
                            w += adv * scale;
                        }
                        return w;
                    };

                    const bool centered = (align == "center");
                    const float blockH = lineAdvance * static_cast<float>(lines.size());
                    const float yTop = centered ? (blockH * 0.5f - ascentI * scale) : 0.0f;

                    Geometry out;
                    // Vertex-class N + uv (faceted text). Declared up front so
                    // the per-glyph addTriangle calls populate them in lockstep.
                    std::vector<Vec3> vN;  // one per emitted vertex (3 per triangle)
                    std::vector<Vec2> vUV;

                    auto emit = [&](uint32_t a, uint32_t b, uint32_t c,
                                    const Vec3 &n, const Vec2 &uva, const Vec2 &uvb, const Vec2 &uvc) {
                        out.addTriangle(a, b, c);
                        vN.push_back(n); vN.push_back(n); vN.push_back(n);
                        vUV.push_back(uva); vUV.push_back(uvb); vUV.push_back(uvc);
                    };

                    const float invSize = size > 0.0f ? 1.0f / size : 1.0f;
                    auto uvOf = [&](const Pt2 &p) { return Vec2(p[0] * invSize, p[1] * invSize); };

                    for (size_t li = 0; li < lines.size(); ++li)
                    {
                        const auto &ln = lines[li];
                        float penX = centered ? -0.5f * lineWidth(ln) : 0.0f;
                        const float penY = yTop - lineAdvance * static_cast<float>(li);
                        uint32_t prev = 0;
                        for (uint32_t cp : ln)
                        {
                            int adv, lsb;
                            stbtt_GetCodepointHMetrics(&font, static_cast<int>(cp), &adv, &lsb);
                            if (prev)
                                penX += stbtt_GetCodepointKernAdvance(
                                            &font, static_cast<int>(prev), static_cast<int>(cp)) * scale;

                            auto contours = glyphContours(font, static_cast<int>(cp), scale, tol);
                            if (!contours.empty())
                                emitGlyph(out, contours, penX, penY, depth, emit, uvOf);

                            penX += adv * scale;
                            prev = cp;
                        }
                    }

                    if (out.vertexCount() == 0) return {};

                    // Bulk-assign vertex attrs now that the table is sized.
                    auto *nAttr = out.vertices().add<Vec3>("N", Vec3(0.0f, 0.0f, 1.0f));
                    auto *uvAttr = out.vertices().add<Vec2>("uv", Vec2(0.0f));
                    for (size_t i = 0; i < vN.size() && i < nAttr->data().size(); ++i)
                    {
                        nAttr->data()[i] = vN[i];
                        uvAttr->data()[i] = vUV[i];
                    }
                    return out;
                }

            private:
                template <typename EmitFn, typename UvFn>
                static void emitGlyph(Geometry &out, std::vector<Contour> &contours,
                                      float penX, float penY, float depth,
                                      EmitFn &emit, UvFn &uvOf)
                {
                    const size_t nc = contours.size();
                    // Classify each contour: hole iff contained by an odd number
                    // of others. Then group each hole under the smallest-area
                    // outer that contains it.
                    std::vector<bool> isHole(nc, false);
                    std::vector<float> area(nc);
                    for (size_t i = 0; i < nc; ++i) area[i] = signedArea(contours[i]);
                    for (size_t i = 0; i < nc; ++i)
                    {
                        int containedBy = 0;
                        for (size_t k = 0; k < nc; ++k)
                            if (k != i && pointInPolygon(contours[i][0], contours[k]))
                                ++containedBy;
                        isHole[i] = (containedBy % 2) == 1;
                    }

                    // Canonical winding: outer CCW (area>0), hole CW (area<0).
                    for (size_t i = 0; i < nc; ++i)
                    {
                        const bool wantCCW = !isHole[i];
                        if ((area[i] > 0.0f) != wantCCW)
                        {
                            std::reverse(contours[i].begin(), contours[i].end());
                            area[i] = -area[i];
                        }
                    }

                    // Build groups: outer index -> [outer, holes...].
                    std::vector<std::vector<size_t>> groups;
                    std::vector<int> outerOf(nc, -1);
                    for (size_t i = 0; i < nc; ++i)
                        if (!isHole[i]) { outerOf[i] = static_cast<int>(groups.size()); groups.push_back({i}); }
                    for (size_t i = 0; i < nc; ++i)
                    {
                        if (!isHole[i]) continue;
                        int best = -1;
                        float bestArea = 1e30f;
                        for (size_t k = 0; k < nc; ++k)
                            if (!isHole[k] && pointInPolygon(contours[i][0], contours[k]) &&
                                std::fabs(area[k]) < bestArea)
                            { best = static_cast<int>(k); bestArea = std::fabs(area[k]); }
                        if (best >= 0) groups[outerOf[best]].push_back(i);
                    }

                    const bool extrude = depth > 1e-6f;
                    for (const auto &grp : groups)
                    {
                        // Flatten the group's rings (outer first) for earcut.
                        std::vector<Contour> rings;
                        rings.reserve(grp.size());
                        for (size_t ci : grp) rings.push_back(contours[ci]);

                        const auto indices = mapbox::earcut<uint32_t>(rings);

                        // Create front (z=0) + back (z=-depth) points for every
                        // ring vertex (flattened in ring order, matching earcut).
                        std::vector<uint32_t> frontPt, backPt;
                        std::vector<Pt2> flat;
                        for (const auto &ring : rings)
                            for (const auto &p : ring) flat.push_back(p);
                        frontPt.reserve(flat.size());
                        backPt.reserve(flat.size());
                        for (const auto &p : flat)
                        {
                            frontPt.push_back(static_cast<uint32_t>(
                                out.addPoint(Vec3(p[0] + penX, p[1] + penY, 0.0f))));
                            if (extrude)
                                backPt.push_back(static_cast<uint32_t>(
                                    out.addPoint(Vec3(p[0] + penX, p[1] + penY, -depth))));
                        }

                        // Front + back faces from the earcut triangulation.
                        for (size_t t = 0; t + 2 < indices.size(); t += 3)
                        {
                            uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
                            // Force +Z winding for the front face.
                            const Pt2 &a = flat[i0], &b = flat[i1], &c = flat[i2];
                            const float cross = (b[0] - a[0]) * (c[1] - a[1]) -
                                                (b[1] - a[1]) * (c[0] - a[0]);
                            if (cross < 0.0f) std::swap(i1, i2);
                            emit(frontPt[i0], frontPt[i1], frontPt[i2], Vec3(0, 0, 1),
                                 uvOf(flat[i0]), uvOf(flat[i1]), uvOf(flat[i2]));
                            if (extrude)
                                emit(backPt[i0], backPt[i2], backPt[i1], Vec3(0, 0, -1),
                                     uvOf(flat[i0]), uvOf(flat[i2]), uvOf(flat[i1]));
                        }

                        // Side walls per ring edge (outer CCW / hole CW so the
                        // single winding rule faces outward for both).
                        if (extrude)
                        {
                            size_t base = 0;
                            for (const auto &ring : rings)
                            {
                                const size_t rn = ring.size();
                                for (size_t k = 0; k < rn; ++k)
                                {
                                    const size_t kn = (k + 1) % rn;
                                    const uint32_t Fk = frontPt[base + k];
                                    const uint32_t Fkn = frontPt[base + kn];
                                    const uint32_t Bk = backPt[base + k];
                                    const uint32_t Bkn = backPt[base + kn];
                                    const float ex = ring[kn][0] - ring[k][0];
                                    const float ey = ring[kn][1] - ring[k][1];
                                    const Vec3 wn = glm::normalize(Vec3(ey, -ex, 0.0f));
                                    emit(Fk, Bk, Bkn, wn, Vec2(0.0f), Vec2(0.0f), Vec2(0.0f));
                                    emit(Fk, Bkn, Fkn, wn, Vec2(0.0f), Vec2(0.0f), Vec2(0.0f));
                                }
                                base += rn;
                            }
                        }
                    }
                }
            };
        }

        void registerMoTextSop()
        {
            SopRegistry::instance().registerType(
                {"mo_text", "MoText", "Generators",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"font_file", ParamType::String, "\"/System/Library/Fonts/Helvetica.ttc\""},
                     {"text", ParamType::String, "\"Text\""},
                     {"size", ParamType::Float, "1.0", 0.05, 50.0, 0.01},
                     {"depth", ParamType::Float, "0.2", 0.0, 10.0, 0.01},
                     {"tolerance", ParamType::Float, "0.002", 0.0001, 0.1, 0.0001},
                     ParamSpec{"align", ParamType::String, "\"center\"", 0.0, 0.0, 0.0, {"left", "center"}},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<MoTextSop>(uid);
                });
        }
    }
}
