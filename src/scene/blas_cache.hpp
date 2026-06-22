#pragma once

#include "../core/types.hpp"
#include "../device/bottom_level_acceleration_structure.hpp"
#include "../device/buffer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracey
{
    // Cache of compiled per-SceneObject GPU resources, keyed by object name.
    //
    // The expensive parts of SceneCompiler::compile are the BLAS build and
    // the vertex / color buffer upload. Those depend only on the object's
    // positions + indices — material, transform, and TLAS membership are
    // independent. When the SOP graph re-cooks but a particular object's
    // geometry didn't change, the cache hands SceneCompiler the existing
    // BLAS + buffers and the recompile becomes just "rebuild the TLAS",
    // which is orders of magnitude cheaper than per-object BVH builds.
    //
    // Lifetime: the cache shares ownership of the GPU resources via shared_ptr.
    // CompiledScene stores raw observer pointers into the cache for entries it
    // consumed AND retains a shared_ptr to each (retainedBuffers/retainedBlases)
    // so the resources outlive the cache entry. Callers
    // (RenderEngine::compile_scene) call markAllUntouched() before compile and
    // evictUntouched() after; an evicted entry drops the CACHE's reference, but
    // any in-flight render snapshot still holding the shared_ptr keeps the
    // buffer alive until that snapshot is gone. This is what stops the async
    // render worker from binding a freed vertex buffer (getMTLBuffer segfault)
    // when a cook/import recompiles the scene mid-render.
    class BlasCache
    {
    public:
        struct Entry
        {
            std::shared_ptr<BottomLevelAccelerationStructure> blas;
            std::shared_ptr<Buffer> vertexBuffer;
            std::shared_ptr<Buffer> colorBuffer;
            size_t vertexCount = 0;
            // UVs travel with the cache entry rather than the GPU because the
            // compiler concatenates them into a global uvBuffer per compile —
            // we keep them around so a cache hit doesn't have to re-extract
            // from the SceneObject.
            std::vector<Vec2> uvs;
            bool hasUvs = false;
            // Per-vertex normals — same per-cook-concat treatment as UVs so
            // the hit shader can interpolate them at intersection. Empty +
            // hasNormals=false when the source SceneObject had no N (the
            // shader then falls back to the face normal stored in the BLAS).
            std::vector<Vec3> normals;
            bool hasNormals = false;
            uint64_t contentHash = 0;
            // Set true when lookup() / insert() returns this entry during a
            // compile, cleared by markAllUntouched(). evictUntouched() drops
            // anything still false.
            bool touched = false;
        };

        // Returns the cached entry for `name` iff its stored contentHash
        // matches the provided one. Side effect: marks it touched. Returns
        // nullptr on miss (different hash, or no entry).
        Entry *lookup(const std::string &name, uint64_t contentHash);

        // Insert a freshly built entry, replacing any existing same-named
        // entry. Marks it touched. Returns a pointer to the stored entry.
        Entry *insert(const std::string &name, Entry entry);

        void markAllUntouched();
        void evictUntouched();
        void clear() { m_entries.clear(); }

        size_t size() const { return m_entries.size(); }

    private:
        std::unordered_map<std::string, Entry> m_entries;
    };
}
