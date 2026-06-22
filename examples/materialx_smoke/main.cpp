// Smoke test for the MaterialX importer (R3f): load a .mtlx, map its
// standard_surface materials onto the engine BSDF, and dump the result.
//
//   materialx_smoke <file.mtlx>

#include "scene/materialx_loader.hpp"

#include <iomanip>
#include <iostream>

using namespace tracey;

static void dumpFloat(const MaterialInstance &m, const char *key)
{
    if (auto v = m.getFloat(key)) std::cout << "    " << key << " = " << *v << "\n";
}

static void dumpVec3(const MaterialInstance &m, const char *key)
{
    if (auto v = m.getVec3(key))
        std::cout << "    " << key << " = (" << v->x << ", " << v->y << ", " << v->z << ")\n";
}

static void dumpTex(const MaterialInstance &m, const char *key)
{
    if (auto t = m.getTexture(key)) std::cout << "    tex[" << key << "] = " << *t << "\n";
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: materialx_smoke <file.mtlx>\n";
        return 2;
    }
    std::cout << "MaterialX support: " << (MaterialXLoader::available() ? "yes" : "no") << "\n";

    auto mats = MaterialXLoader::loadMaterials(argv[1]);
    std::cout << "Loaded " << mats.size() << " material(s) from " << argv[1] << "\n";
    std::cout << std::fixed << std::setprecision(4);

    for (const auto &nm : mats)
    {
        std::cout << "  material '" << nm.name << "':\n";
        dumpVec3(nm.material, "albedo");
        dumpFloat(nm.material, "metallic");
        dumpFloat(nm.material, "roughness");
        dumpFloat(nm.material, "transmission");
        dumpFloat(nm.material, "ior");
        dumpVec3(nm.material, "emission");
        dumpFloat(nm.material, "emissionStrength");
        dumpFloat(nm.material, "clearcoat");
        dumpFloat(nm.material, "clearcoatRoughness");
        dumpFloat(nm.material, "sheen");
        dumpFloat(nm.material, "subsurface");
        dumpVec3(nm.material, "subsurfaceColor");
        dumpFloat(nm.material, "anisotropy");
        dumpFloat(nm.material, "opacity");
        dumpTex(nm.material, TEXTURE_ALBEDO);
        dumpTex(nm.material, TEXTURE_EMISSIVE);
        dumpTex(nm.material, TEXTURE_NORMAL);
    }

    return mats.empty() ? 1 : 0;
}
