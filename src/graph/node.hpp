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

    private:
        size_t m_uid;
    };

    template <typename T>
    concept IsNode = requires(T &t) {
        { std::derived_from<T, Node> };
    };
}