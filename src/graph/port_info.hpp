#pragma once
#include <string_view>
#include <vector>
#include <span>
#include "data_type.hpp"
namespace tracey
{
    enum class PortType
    {
        Input,
        Output
    };

    class PortInfo
    {
    public:
        PortInfo(std::string_view name, PortType type, DataType dataType)
            : name(name), type(type), dataType(dataType)
        {
        }

        static PortInfo createInput(std::string_view name, DataType dataType)
        {
            return PortInfo{name, PortType::Input, dataType};
        }

        static PortInfo createOutput(std::string_view name, DataType dataType)
        {
            return PortInfo{name, PortType::Output, dataType};
        }

    private:
        std::string_view name;
        PortType type;
        DataType dataType;
    };

    class InputsAndOutputs
    {
    public:
        void addInput(const PortInfo &port)
        {
            m_inputs.emplace_back(port);
        }

        void addOutput(const PortInfo &port)
        {
            m_outputs.emplace_back(port);
        }

        const std::span<const PortInfo> inputs() const
        {
            return m_inputs;
        }

        const std::span<const PortInfo> outputs() const
        {
            return m_outputs;
        }

    private:
        std::vector<PortInfo> m_inputs;
        std::vector<PortInfo> m_outputs;
    };
}