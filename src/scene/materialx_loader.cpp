#include "materialx_loader.hpp"

#include <iostream>

#ifdef TRACEY_HAS_MATERIALX

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Node.h>
#include <MaterialXCore/Types.h>
#include <MaterialXCore/Value.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include <filesystem>

namespace mx = MaterialX;

namespace tracey
{
    namespace
    {
        // Resolve an input's value, falling back to the node definition's
        // default when the instance doesn't override it. Returns null when the
        // input is connected to a graph (no constant value) or absent.
        mx::ValuePtr resolveValue(const mx::NodePtr &node, const mx::NodeDefPtr &nodeDef,
                                  const std::string &name)
        {
            if (auto in = node->getInput(name))
                if (in->hasValue()) return in->getValue();
            if (nodeDef)
                if (auto din = nodeDef->getInput(name))
                    if (din->hasValue()) return din->getValue();
            return nullptr;
        }

        float asFloat(const mx::ValuePtr &v, float fallback)
        {
            if (v && v->isA<float>()) return v->asA<float>();
            return fallback;
        }

        Vec3 asColor(const mx::ValuePtr &v, const Vec3 &fallback)
        {
            if (v && v->isA<mx::Color3>())
            {
                const mx::Color3 c = v->asA<mx::Color3>();
                return Vec3(c[0], c[1], c[2]);
            }
            // Some inputs (e.g. opacity) may be authored as a float — broadcast.
            if (v && v->isA<float>())
            {
                const float f = v->asA<float>();
                return Vec3(f, f, f);
            }
            return fallback;
        }

        // If `inputName` connects directly to an <image>/<tiledimage> node,
        // return its resolved (absolute) file path; empty otherwise. Procedural
        // (non-image) connections are left for the node-graph VM compiler.
        std::string connectedImageFile(const mx::NodePtr &node, const std::string &inputName,
                                       const std::filesystem::path &baseDir)
        {
            auto in = node->getInput(inputName);
            if (!in) return {};
            mx::NodePtr cn = in->getConnectedNode();
            if (!cn) return {};
            const std::string cat = cn->getCategory();
            if (cat != "image" && cat != "tiledimage") return {};
            auto fileIn = cn->getInput("file");
            if (!fileIn) return {};
            const std::string file = fileIn->getValueString();
            if (file.empty()) return {};
            std::filesystem::path p(file);
            if (p.is_absolute()) return p.string();
            return (baseDir / p).lexically_normal().string();
        }

        // Map one MaterialX standard_surface shader node onto a MaterialInstance.
        MaterialInstance mapStandardSurface(const mx::NodePtr &shader,
                                            const std::filesystem::path &baseDir)
        {
            const mx::NodeDefPtr nd = shader->getNodeDef();
            MaterialInstance m("pbr");

            auto F = [&](const char *name, float def) {
                return asFloat(resolveValue(shader, nd, name), def);
            };
            auto C = [&](const char *name, const Vec3 &def) {
                return asColor(resolveValue(shader, nd, name), def);
            };

            // Base / diffuse — standard_surface diffuse albedo is base * base_color.
            const float base = F("base", 1.0f);
            const Vec3 baseColor = C("base_color", Vec3(0.8f));
            m.setAlbedo(base * baseColor);
            m.setMetallic(F("metalness", 0.0f));
            m.setRoughness(F("specular_roughness", 0.2f));

            // Transmission / dielectric.
            m.setFloat("transmission", F("transmission", 0.0f));
            m.setFloat("ior", F("specular_IOR", 1.5f));

            // Emission — the weight scales the emission color (our HDR model).
            const float emissionWeight = F("emission", 0.0f);
            m.setEmission(C("emission_color", Vec3(1.0f)));
            m.setFloat("emissionStrength", emissionWeight);

            // Coat → our clearcoat lobe.
            m.setFloat("clearcoat", F("coat", 0.0f));
            m.setFloat("clearcoatRoughness", F("coat_roughness", 0.1f));

            // Sheen / subsurface / anisotropy → the R3 lobes.
            m.setFloat("sheen", F("sheen", 0.0f));
            m.setFloat("subsurface", F("subsurface", 0.0f));
            m.setVec3("subsurfaceColor", C("subsurface_color", Vec3(1.0f)));
            m.setFloat("anisotropy", F("specular_anisotropy", 0.0f));

            // Opacity (authored as color3 or float).
            const Vec3 op = C("opacity", Vec3(1.0f));
            m.setFloat("opacity", op.x);

            // Direct image connections → texture slots (the common authoring
            // pattern). Procedural node graphs feeding these inputs are handled
            // by the node-graph VM compiler (R3f.2).
            if (auto t = connectedImageFile(shader, "base_color", baseDir); !t.empty())
                m.setTexture(TEXTURE_ALBEDO, t);
            if (auto t = connectedImageFile(shader, "emission_color", baseDir); !t.empty())
                m.setTexture(TEXTURE_EMISSIVE, t);
            if (auto t = connectedImageFile(shader, "normal", baseDir); !t.empty())
                m.setTexture(TEXTURE_NORMAL, t);

            return m;
        }

        // Collect renderable surface shader nodes. Prefer the surfaceshader
        // connected to each <surfacematerial>; fall back to standalone
        // standard_surface nodes when no material node wraps them.
        std::vector<std::pair<std::string, mx::NodePtr>> collectShaders(const mx::DocumentPtr &doc)
        {
            std::vector<std::pair<std::string, mx::NodePtr>> out;
            for (const mx::NodePtr &matNode : doc->getMaterialNodes())
            {
                mx::NodePtr shader = matNode->getConnectedNode("surfaceshader");
                if (shader && shader->getCategory() == "standard_surface")
                    out.emplace_back(matNode->getName(), shader);
            }
            if (out.empty())
                for (const mx::NodePtr &n : doc->getNodes("standard_surface"))
                    out.emplace_back(n->getName(), n);
            return out;
        }
    }

    bool MaterialXLoader::available() { return true; }

    std::vector<MaterialXLoader::NamedMaterial>
    MaterialXLoader::loadMaterials(const std::string &path)
    {
        std::vector<NamedMaterial> result;
        try
        {
            mx::DocumentPtr doc = mx::createDocument();

            // Load the standard data libraries so node defs (and their input
            // defaults) resolve. TRACEY_MATERIALX_LIBRARIES_DIR is baked in by
            // CMake; it points at deps/materialx/libraries.
            mx::DocumentPtr stdlib = mx::createDocument();
            mx::FileSearchPath libSearch(std::string(TRACEY_MATERIALX_LIBRARIES_DIR));
            mx::loadLibraries({"stdlib", "pbrlib", "bxdf", "targets"}, libSearch, stdlib);
            doc->importLibrary(stdlib);

            const std::filesystem::path baseDir =
                std::filesystem::path(path).parent_path();
            mx::FileSearchPath docSearch(baseDir.string());
            mx::readFromXmlFile(doc, path, docSearch);

            for (auto &[name, shader] : collectShaders(doc))
                result.push_back({name, mapStandardSurface(shader, baseDir)});

            if (result.empty())
                std::cerr << "[materialx] no standard_surface materials in " << path << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[materialx] failed to load " << path << ": " << e.what() << std::endl;
            return {};
        }
        return result;
    }
}

#else // !TRACEY_HAS_MATERIALX

namespace tracey
{
    bool MaterialXLoader::available() { return false; }

    std::vector<MaterialXLoader::NamedMaterial>
    MaterialXLoader::loadMaterials(const std::string &path)
    {
        std::cerr << "[materialx] build has no MaterialX support; cannot load "
                  << path << " (run scripts/bootstrap_deps.sh and rebuild)." << std::endl;
        return {};
    }
}

#endif
