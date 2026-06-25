#include "skeleton.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <functional>

namespace tracey
{
    namespace
    {
        // Sample one channel at time `t` (seconds). Clamps to the endpoints
        // outside the keyed range. Rotation slerps; translation/scale lerp.
        // Caller guarantees times is non-empty and parallel to values.
        Vec4 sampleChannel(const AnimChannel &ch, double t)
        {
            const auto &times = ch.times;
            const auto &vals = ch.values;
            if (t <= times.front())
                return vals.front();
            if (t >= times.back())
                return vals.back();

            // First key strictly greater than t → the segment is (b-1, b).
            const size_t b = static_cast<size_t>(
                std::upper_bound(times.begin(), times.end(), static_cast<float>(t)) - times.begin());
            const size_t a = b - 1;

            if (ch.step)
                return vals[a];

            const float span = times[b] - times[a];
            const float u = span > 1e-9f
                                ? static_cast<float>((t - times[a]) / span)
                                : 0.0f;

            if (ch.path == AnimChannel::Path::Rotation)
            {
                const glm::quat qa(vals[a].w, vals[a].x, vals[a].y, vals[a].z);
                const glm::quat qb(vals[b].w, vals[b].x, vals[b].y, vals[b].z);
                const glm::quat q = glm::slerp(qa, qb, u);
                return Vec4(q.x, q.y, q.z, q.w);
            }
            return glm::mix(vals[a], vals[b], u);
        }
    }

    std::vector<Mat4> Skeleton::computeNodeWorlds(double t, size_t clipIndex,
                                                  const PoseOverrides* overrides) const
    {
        const size_t nn = nodes.size();

        // 1. Start from the bind-pose local TRS for every node.
        std::vector<Vec3> T(nn);
        std::vector<glm::quat> R(nn);
        std::vector<Vec3> S(nn);
        for (size_t i = 0; i < nn; ++i)
        {
            T[i] = nodes[i].t;
            R[i] = nodes[i].r;
            S[i] = nodes[i].s;
        }

        // 2. Overlay the clip's keyed components at time t. Un-keyed components
        //    (and un-keyed nodes) keep their bind value.
        if (clipIndex < clips.size())
        {
            for (const auto &ch : clips[clipIndex].channels)
            {
                if (ch.node < 0 || static_cast<size_t>(ch.node) >= nn || ch.times.empty())
                    continue;
                const Vec4 v = sampleChannel(ch, t);
                switch (ch.path)
                {
                case AnimChannel::Path::Translation:
                    T[ch.node] = Vec3(v);
                    break;
                case AnimChannel::Path::Scale:
                    S[ch.node] = Vec3(v);
                    break;
                case AnimChannel::Path::Rotation:
                    R[ch.node] = glm::quat(v.w, v.x, v.y, v.z);
                    break;
                }
            }
        }

        // 2b. FK pose overrides: post-multiply each node's local rotation by the
        //     user delta (R = R_clip * delta), so hand-posing composes on top of
        //     the clip/bind pose in the joint's local frame.
        if (overrides)
        {
            const size_t on = overrides->size();
            for (size_t i = 0; i < nn && i < on; ++i)
                R[i] = R[i] * (*overrides)[i];
        }

        // 3. Compose per-node local matrices (T * R * S).
        std::vector<Mat4> local(nn);
        for (size_t i = 0; i < nn; ++i)
        {
            Mat4 m = glm::translate(Mat4(1.0f), T[i]);
            m *= glm::mat4_cast(R[i]);
            m = glm::scale(m, S[i]);
            local[i] = m;
        }

        // 4. World matrices via a memoized parent walk.
        std::vector<Mat4> world(nn, Mat4(1.0f));
        std::vector<char> done(nn, 0);
        std::function<Mat4(int)> worldOf = [&](int i) -> Mat4
        {
            if (i < 0 || static_cast<size_t>(i) >= nn)
                return Mat4(1.0f);
            if (done[i])
                return world[i];
            done[i] = 1; // set before recursing so a malformed cyclic parent can't loop forever
            const Mat4 w = (nodes[i].parent >= 0)
                               ? worldOf(nodes[i].parent) * local[i]
                               : local[i];
            world[i] = w;
            return w;
        };
        for (size_t i = 0; i < nn; ++i)
            worldOf(static_cast<int>(i));
        return world;
    }

    std::vector<Mat4> Skeleton::skinningMatrices(double t, size_t clipIndex,
                                                 const PoseOverrides* overrides) const
    {
        const std::vector<Mat4> world = computeNodeWorlds(t, clipIndex, overrides);
        const size_t nn = world.size();

        // Skinning matrix per joint: worldOf(joint) * inverseBind.
        std::vector<Mat4> skin(joints.size(), Mat4(1.0f));
        for (size_t k = 0; k < joints.size(); ++k)
        {
            const int jn = joints[k];
            const Mat4 jw = (jn >= 0 && static_cast<size_t>(jn) < nn) ? world[jn] : Mat4(1.0f);
            const Mat4 ib = (k < inverseBind.size()) ? inverseBind[k] : Mat4(1.0f);
            skin[k] = jw * ib;
        }
        return skin;
    }

    std::vector<Mat4> Skeleton::jointWorldMatrices(double t, size_t clipIndex,
                                                   const PoseOverrides* overrides) const
    {
        const std::vector<Mat4> world = computeNodeWorlds(t, clipIndex, overrides);
        const size_t nn = world.size();
        std::vector<Mat4> out(joints.size(), Mat4(1.0f));
        for (size_t k = 0; k < joints.size(); ++k)
        {
            const int jn = joints[k];
            out[k] = (jn >= 0 && static_cast<size_t>(jn) < nn) ? world[jn] : Mat4(1.0f);
        }
        return out;
    }

    std::vector<int> Skeleton::jointParents() const
    {
        // Map node index → joint index (-1 if the node isn't a joint).
        std::vector<int> nodeToJoint(nodes.size(), -1);
        for (size_t k = 0; k < joints.size(); ++k)
            if (joints[k] >= 0 && static_cast<size_t>(joints[k]) < nodes.size())
                nodeToJoint[joints[k]] = static_cast<int>(k);

        std::vector<int> parents(joints.size(), -1);
        for (size_t k = 0; k < joints.size(); ++k)
        {
            int n = (joints[k] >= 0 && static_cast<size_t>(joints[k]) < nodes.size())
                        ? nodes[joints[k]].parent
                        : -1;
            // Walk up node parents to the nearest ancestor that is also a joint.
            while (n >= 0 && static_cast<size_t>(n) < nodes.size())
            {
                if (nodeToJoint[n] >= 0)
                {
                    parents[k] = nodeToJoint[n];
                    break;
                }
                n = nodes[n].parent;
            }
        }
        return parents;
    }

    Mat4 Skeleton::nodeBindWorld(int node) const
    {
        if (node < 0 || static_cast<size_t>(node) >= nodes.size())
            return Mat4(1.0f);
        const SkelNode &n = nodes[node];
        const Mat4 local = glm::translate(Mat4(1.0f), n.t) *
                           glm::mat4_cast(n.r) *
                           glm::scale(Mat4(1.0f), n.s);
        return (n.parent >= 0) ? nodeBindWorld(n.parent) * local : local;
    }
}
