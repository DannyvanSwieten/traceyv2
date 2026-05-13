#pragma once
#include <climits>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "light.hpp"
#include "transform.hpp"
#include "scene_instance.hpp"

namespace tracey
{
    class Scene;
    class Actor
    {
    public:
        ~Actor() = default;

        Actor(const Actor &) = delete;
        Actor &operator=(const Actor &) = delete;

        const std::string &name() const { return m_name; }
        void setName(const std::string &name) { m_name = name; }

        size_t getUid() const { return uid; }

        // The transform stored here is a *local* transform; Scene::flatten
        // composes world = parent.world × actor.local while walking the
        // parent → children tree. SceneCompiler consumes the resulting world
        // transforms when building TLAS instances.
        void setTransform(const Transform &transform);
        void applyTransform(const Transform &deltaTransform);
        const Transform &transform() const;

        // Parent–child topology. Parent uid is SIZE_MAX when this actor sits
        // at the root of the scene tree. addChild() appends the child's uid
        // to m_children but does NOT set the child's parent — call setParent
        // on the child too. (Both edges are stored so Scene::flatten can run
        // top-down without a per-actor reverse scan.)
        static constexpr size_t kNoParent = SIZE_MAX;
        size_t parent() const { return m_parent; }
        void setParent(size_t parentUid) { m_parent = parentUid; }
        bool hasParent() const { return m_parent != kNoParent; }

        const std::span<const size_t> children() const;
        void addChild(Actor *child)
        {
            m_children.push_back(child->uid);
        }
        void removeChild(size_t childUid);

        // Instance management
        void addInstance(const SceneInstance &instance) { m_instances.push_back(instance); }
        void addInstance(SceneInstance &&instance) { m_instances.push_back(std::move(instance)); }
        const std::vector<SceneInstance> &instances() const { return m_instances; }
        std::vector<SceneInstance> &instances() { return m_instances; }
        void clearInstances() { m_instances.clear(); }

        // ShaderGraph JSON attached to this actor. Empty string means
        // "use the default passthrough program". SceneCompiler aggregates
        // unique JSON strings across actors into the MaterialProgramBuffer
        // and emits one program per unique graph. The editor populates this
        // by reading from its per-user material library (a name -> file map
        // editor-side); the engine itself doesn't know about that catalog.
        const std::string &materialGraphJson() const { return m_materialGraphJson; }
        void setMaterialGraphJson(const std::string &json) { m_materialGraphJson = json; }

        // Name of the library entry this actor's material came from, when
        // the user bound it via set_actor_material or the SOP's
        // material_library_name param. Empty means "no library binding"
        // (passthrough, glTF source, or scratchpad). Stored so editor-side
        // "save material" can find every actor that needs to reload its
        // JSON from disk without re-cooking the SOP graph.
        const std::string &materialLibraryName() const { return m_materialLibraryName; }
        void setMaterialLibraryName(const std::string &name) { m_materialLibraryName = name; }

        // Per-actor display flag. Hidden actors are skipped entirely by the
        // SceneCompiler, so they appear in neither the path tracer's TLAS nor
        // the rasterizer's compiled instance list. Defaults to visible.
        bool visible() const { return m_visible; }
        void setVisible(bool v) { m_visible = v; }

        // Optional light component (Houdini-style /obj light). When set, the
        // SceneCompiler emits this actor into its light list; the actor may
        // still carry SceneInstances for visualisation gizmos, but the bulk
        // of light actors are transform-only.
        bool hasLight() const { return m_light.has_value(); }
        const Light *light() const { return m_light ? &*m_light : nullptr; }
        Light *light() { return m_light ? &*m_light : nullptr; }
        void setLight(const Light &l) { m_light = l; }
        void clearLight() { m_light.reset(); }

        Actor(Scene *scene, size_t uid) : m_scene(scene), uid(uid) {}

    private:
        [[maybe_unused]] Scene *m_scene = nullptr;
        size_t uid = 0;
        std::string m_name;
        Transform m_transform;
        size_t m_parent = kNoParent;
        std::vector<size_t> m_children;
        std::vector<SceneInstance> m_instances;
        std::string m_materialGraphJson;
        std::string m_materialLibraryName;
        bool m_visible = true;
        std::optional<Light> m_light;
    };
}