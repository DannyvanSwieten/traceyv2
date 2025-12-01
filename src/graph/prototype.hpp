#pragma once
#include "port_info.hpp"
#include "template.hpp"
#include <memory>
namespace tracey
{
    class Node;
    class Prototype
    {
    public:
        Prototype(std::string name, size_t version)
            : m_name(std::move(name)), m_version(version)
        {
        }
        virtual std::unique_ptr<Node> create() const = 0;
        virtual Template defaultTemplate() const = 0;
        virtual InputsAndOutputs instantiate(const Template &t) const = 0;

    private:
        std::string m_name;
        std::string m_description;
        size_t m_version = 0;
    };
}