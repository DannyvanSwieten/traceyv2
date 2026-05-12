#include "parameter.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Cubic Hermite using outTangent of `a` and inTangent of `b`,
            // scaled to the segment length so tangents stay in (value/sec).
            float interpolateBezier(const ScalarChannel::Key &a,
                                    const ScalarChannel::Key &b,
                                    double t)
            {
                const double dt = b.time - a.time;
                const double t2 = t * t;
                const double t3 = t2 * t;
                const float h00 = float( 2.0 * t3 - 3.0 * t2 + 1.0);
                const float h10 = float(       t3 - 2.0 * t2 + t  );
                const float h01 = float(-2.0 * t3 + 3.0 * t2      );
                const float h11 = float(       t3 -       t2      );
                return h00 * a.value
                     + h10 * float(dt) * a.outTangent
                     + h01 * b.value
                     + h11 * float(dt) * b.inTangent;
            }

            float interpolateLinear(const ScalarChannel::Key &a,
                                    const ScalarChannel::Key &b,
                                    double t)
            {
                return a.value + float(t) * (b.value - a.value);
            }

            // Wrap `time` into [first.time, last.time] for Cycle extrapolation.
            // Returns true if the input was inside the bounded range and `time`
            // is unchanged; false (with `time` updated) when wrapping occurred.
            void wrapCycle(double &time, double first, double last)
            {
                const double range = last - first;
                if (range <= 0.0) { time = first; return; }
                double off = std::fmod(time - first, range);
                if (off < 0.0) off += range;
                time = first + off;
            }
        }

        float ScalarChannel::evaluate(double time) const
        {
            if (keys.empty()) return 0.0f;
            if (keys.size() == 1) return keys.front().value;

            const auto &first = keys.front();
            const auto &last  = keys.back();

            if (time < first.time)
            {
                switch (pre)
                {
                case Extrap::Hold:
                    return first.value;
                case Extrap::Linear:
                {
                    const double dt = first.time - time;
                    return first.value - float(dt) * first.inTangent;
                }
                case Extrap::Cycle:
                    wrapCycle(time, first.time, last.time);
                    break;
                }
            }
            if (time > last.time)
            {
                switch (post)
                {
                case Extrap::Hold:
                    return last.value;
                case Extrap::Linear:
                {
                    const double dt = time - last.time;
                    return last.value + float(dt) * last.outTangent;
                }
                case Extrap::Cycle:
                    wrapCycle(time, first.time, last.time);
                    break;
                }
            }

            // Bracket: find first key with key.time > time. The previous key
            // is the segment start, this one is the segment end.
            auto it = std::upper_bound(keys.begin(), keys.end(), time,
                [](double t, const Key &k) { return t < k.time; });
            if (it == keys.begin()) return keys.front().value;
            if (it == keys.end())   return keys.back().value;
            const Key &b = *it;
            const Key &a = *(it - 1);

            const double range = b.time - a.time;
            if (range <= 0.0) return a.value;
            const double t = (time - a.time) / range;

            switch (a.interp)
            {
            case Interp::Step:   return a.value;
            case Interp::Linear: return interpolateLinear(a, b, t);
            case Interp::Bezier: return interpolateBezier(a, b, t);
            }
            return a.value;
        }

        void ScalarChannel::setKey(Key k)
        {
            auto it = std::lower_bound(keys.begin(), keys.end(), k.time,
                [](const Key &kk, double t) { return kk.time < t; });
            if (it != keys.end() && std::abs(it->time - k.time) < 1e-9)
            {
                *it = k;
                return;
            }
            keys.insert(it, k);
        }

        bool ScalarChannel::removeKeyAt(double time, double epsilon)
        {
            auto it = std::lower_bound(keys.begin(), keys.end(), time,
                [](const Key &kk, double t) { return kk.time < t; });
            if (it != keys.end() && std::abs(it->time - time) <= epsilon)
            {
                keys.erase(it);
                return true;
            }
            if (it != keys.begin())
            {
                auto prev = it - 1;
                if (std::abs(prev->time - time) <= epsilon)
                {
                    keys.erase(prev);
                    return true;
                }
            }
            return false;
        }

        bool Parameter::isAnimated() const
        {
            for (const auto &c : channels)
                if (!c.keys.empty()) return true;
            return false;
        }

        std::variant<float, int, bool, Vec3, std::string>
        Parameter::evaluateAt(double time) const
        {
            switch (type)
            {
            case ParamType::Float:
                if (!channels.empty() && !channels[0].empty())
                    return channels[0].evaluate(time);
                if (auto *v = std::get_if<float>(&value)) return *v;
                return 0.0f;

            case ParamType::Int:
                if (!channels.empty() && !channels[0].empty())
                    return int(std::lround(channels[0].evaluate(time)));
                if (auto *v = std::get_if<int>(&value)) return *v;
                return 0;

            case ParamType::Bool:
                if (!channels.empty() && !channels[0].empty())
                    return channels[0].evaluate(time) != 0.0f;
                if (auto *v = std::get_if<bool>(&value)) return *v;
                return false;

            case ParamType::Vec3:
            {
                Vec3 v(0.0f);
                if (auto *cv = std::get_if<Vec3>(&value)) v = *cv;
                if (channels.size() > 0 && !channels[0].empty())
                    v.x = channels[0].evaluate(time);
                if (channels.size() > 1 && !channels[1].empty())
                    v.y = channels[1].evaluate(time);
                if (channels.size() > 2 && !channels[2].empty())
                    v.z = channels[2].evaluate(time);
                return v;
            }

            case ParamType::String:
                if (auto *v = std::get_if<std::string>(&value)) return *v;
                return std::string{};
            }
            return value;
        }

        ScalarChannel &Parameter::channelAt(int component)
        {
            if (component < 0) component = 0;
            if (int(channels.size()) <= component)
                channels.resize(component + 1);
            return channels[component];
        }

        const char *interpName(ScalarChannel::Interp i)
        {
            switch (i)
            {
            case ScalarChannel::Interp::Step:   return "step";
            case ScalarChannel::Interp::Linear: return "linear";
            case ScalarChannel::Interp::Bezier: return "bezier";
            }
            return "linear";
        }

        ScalarChannel::Interp interpFromName(std::string_view name,
                                             ScalarChannel::Interp def)
        {
            if (name == "step")   return ScalarChannel::Interp::Step;
            if (name == "linear") return ScalarChannel::Interp::Linear;
            if (name == "bezier") return ScalarChannel::Interp::Bezier;
            return def;
        }

        const char *extrapName(ScalarChannel::Extrap e)
        {
            switch (e)
            {
            case ScalarChannel::Extrap::Hold:   return "hold";
            case ScalarChannel::Extrap::Cycle:  return "cycle";
            case ScalarChannel::Extrap::Linear: return "linear";
            }
            return "hold";
        }

        ScalarChannel::Extrap extrapFromName(std::string_view name,
                                             ScalarChannel::Extrap def)
        {
            if (name == "hold")   return ScalarChannel::Extrap::Hold;
            if (name == "cycle")  return ScalarChannel::Extrap::Cycle;
            if (name == "linear") return ScalarChannel::Extrap::Linear;
            return def;
        }
    }
}
