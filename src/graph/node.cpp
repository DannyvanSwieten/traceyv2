#include "node.hpp"

namespace tracey
{
    Node::Node(size_t uid)
        : m_uid(uid)
    {
    }
    size_t Node::uid() const
    {
        return m_uid;
    }

}