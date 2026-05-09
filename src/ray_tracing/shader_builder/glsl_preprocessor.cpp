#include "glsl_preprocessor.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace tracey
{
    namespace
    {
        std::string preprocessGlslImpl(const std::string &source,
                                       const std::filesystem::path &basePath,
                                       int depth)
        {
            if (depth > 32)
            {
                throw std::runtime_error(
                    "preprocessGlsl: include depth exceeded 32 (circular include?)");
            }

            std::stringstream result;
            std::istringstream sourceStream(source);
            std::string line;

            const std::regex includePattern(R"(^\s*#include\s+\"([^\"]+)\"\s*$)");

            while (std::getline(sourceStream, line))
            {
                std::smatch match;
                if (std::regex_match(line, match, includePattern))
                {
                    const std::string includePath = match[1].str();
                    const std::filesystem::path fullPath = basePath / includePath;

                    std::ifstream includeFile(fullPath);
                    if (!includeFile.is_open())
                    {
                        throw std::runtime_error(
                            "preprocessGlsl: failed to open include '" +
                            fullPath.string() + "'");
                    }

                    std::stringstream buf;
                    buf << includeFile.rdbuf();
                    const std::string included = preprocessGlslImpl(
                        buf.str(), fullPath.parent_path(), depth + 1);

                    result << "// Begin include: " << includePath << "\n";
                    result << included;
                    result << "// End include: " << includePath << "\n";
                }
                else
                {
                    result << line << "\n";
                }
            }

            return result.str();
        }
    }

    std::string preprocessGlsl(const std::string &source,
                               const std::filesystem::path &basePath)
    {
        return preprocessGlslImpl(source, basePath, 0);
    }
}
