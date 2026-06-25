#pragma once

#include "../core/types.hpp"

#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace tracey
{
    // One node in the skeleton's local hierarchy. Indexed by glTF node index
    // (the skeleton stores ALL nodes of the source asset, not just joints, so
    // a joint's world transform can be walked through any non-joint ancestors).
    // Transform is stored as TRS so an animation channel can override one
    // component (e.g. rotation only) while the others keep their bind value.
    struct SkelNode
    {
        int parent = -1;          // parent glTF node index; -1 = root
        Vec3 t{0.0f};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f}; // wxyz, identity
        Vec3 s{1.0f};
    };

    // A single animation channel: a keyframed track targeting one node's
    // translation, rotation, or scale. Times are seconds (sorted ascending).
    // Values are Vec4 — T/S use xyz (w ignored); R uses a full xyzw quaternion.
    struct AnimChannel
    {
        enum class Path
        {
            Translation,
            Rotation,
            Scale
        };

        int node = -1;              // target glTF node index
        Path path = Path::Translation;
        std::vector<float> times;   // seconds, ascending
        std::vector<Vec4> values;   // parallel to times
        bool step = false;          // STEP interpolation (else LINEAR/SLERP)
    };

    // A named animation clip — a set of channels driving node transforms.
    struct AnimationClip
    {
        std::string name;
        float duration = 0.0f;      // seconds (max channel time)
        std::vector<AnimChannel> channels;
    };

    // A skinning skeleton parsed from a glTF skin: the node hierarchy (bind
    // pose), the joint list with inverse-bind matrices, and any animation
    // clips. Self-contained: the skinning deformer needs only this + a time.
    class Skeleton
    {
    public:
        std::vector<SkelNode> nodes;       // indexed by glTF node index (all nodes)
        std::vector<int> joints;           // node indices, in skin joint order
        std::vector<Mat4> inverseBind;     // parallel to joints
        std::vector<AnimationClip> clips;  // optional

        bool empty() const { return joints.empty(); }
        bool animated() const { return !clips.empty(); }

        // FK pose overrides: a per-node local-rotation DELTA (wxyz, identity =
        // none), post-multiplied onto each node's clip/bind local rotation
        // (R = R_clip * delta). Indexed by glTF node index; pass nullptr (the
        // default) for no overrides. Lets the editor pose joints by hand on top
        // of the animation, with the same evaluation feeding the skinning, the
        // bone overlay, and joint picking.
        using PoseOverrides = std::vector<glm::quat>;

        // Skinning matrices at time `t` (seconds) for clip `clipIndex`:
        //   skin[k] = worldOf(joints[k]) * inverseBind[k]
        // Size == joints.size(). With no clips (or an out-of-range index) this
        // evaluates the bind pose — for a correctly authored rig every matrix
        // is the identity, so skinned positions reproduce the original mesh
        // (the bind-pose round-trip the smoke test relies on).
        std::vector<Mat4> skinningMatrices(double t, size_t clipIndex = 0,
                                           const PoseOverrides* overrides = nullptr) const;

        // Bind-pose world matrix of a node (no clip). Identity for an
        // out-of-range index. Used to express skinning in the mesh node's
        // local space: the deformer premultiplies the skinning matrices by
        // inverse(nodeBindWorld(meshNode)) so the engine's actor transform
        // (which already carries the glTF node hierarchy) places the skinned
        // mesh correctly instead of double-transforming it.
        Mat4 nodeBindWorld(int node) const;

        // Posed world transform of each joint at time t (the joint pivots,
        // before inverse-bind). In glTF scene space — which, because the
        // skinned mesh renders at scene-space positions, is exactly where the
        // bones align with the deformed character. For drawing the overlay.
        std::vector<Mat4> jointWorldMatrices(double t, size_t clipIndex = 0,
                                             const PoseOverrides* overrides = nullptr) const;

        // For each joint, the index (into joints[]) of its nearest ancestor
        // joint, or -1 for a root joint. Defines the bone segments to draw
        // (a line from each joint to its parent joint).
        std::vector<int> jointParents() const;

    private:
        // Posed world matrix for every node at time t (clip + FK overrides
        // applied, then a memoized parent walk). Shared by skinningMatrices and
        // jointWorldMatrices.
        std::vector<Mat4> computeNodeWorlds(double t, size_t clipIndex,
                                            const PoseOverrides* overrides) const;
    };
}
