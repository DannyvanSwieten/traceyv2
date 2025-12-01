#pragma once
#include <string>
#include <span>
#include <vector>
#include <optional>
#include "data_type.hpp"
namespace tracey
{
    struct Argument
    {
        DataType type;
    };
    class Template
    {
    public:
        Template() = default;
        Template(std::span<const Argument> args)
            : m_args(args.begin(), args.end())
        {
        }
        Template(std::initializer_list<Argument> args)
            : m_args(args)
        {
        }
        void addArgument(const Argument &arg)
        {
            m_args.emplace_back(arg);
        }
        const std::optional<Argument> operator[](size_t index) const
        {
            if (index < m_args.size())
                return m_args[index];

            return std::nullopt;
        }

    private:
        std::vector<Argument> m_args;
    };
}