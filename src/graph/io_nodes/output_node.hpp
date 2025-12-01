#pragma once
#include "../prototype.hpp"
#include "../node.hpp"
namespace tracey
{
    class OutputNode : public Node
    {
    public:
        OutputNode(size_t uid);
    };

    class OutputPrototype : public Prototype
    {
    public:
        OutputPrototype(std::string name, size_t version, DataType dataType);

        std::unique_ptr<Node> create() const override;
        Template defaultTemplate() const override;
        InputsAndOutputs instantiate(const Template &t) const override;

    private:
        DataType m_dataType;
    };
}