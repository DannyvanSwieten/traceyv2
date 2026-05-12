#pragma once
#include "port_info.hpp"
namespace tracey
{
    class InputsAndOutputs;
    class Node
    {
    public:
        Node(size_t uid);
        virtual ~Node() = default;
        size_t uid() const;

        // Bypass flag — when true, the node's cook is skipped and its first
        // input passes straight through to the output. Used for "disable
        // this transformation but keep the wire in place" debugging and
        // for A/B-comparing with-and-without an effect. Stored on the base
        // so SopGraph::cook can honour it uniformly without each SOP type
        // having to opt in. Defaults to false; serialised only when true
        // to keep clean graphs JSON-compact.
        bool bypass() const { return m_bypass; }
        void setBypass(bool b) { m_bypass = b; }

    private:
        size_t m_uid;
        bool m_bypass = false;
    };

    template <typename T>
    concept IsNode = requires(T &t) {
        { std::derived_from<T, Node> };
    };
}