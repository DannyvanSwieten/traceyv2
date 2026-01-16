#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include "../../device/device.hpp"

namespace tracey {

struct ISFInput {
    std::string name;
    std::string type; // "float", "color", "point2D", "bool", "long"
    std::variant<float, glm::vec4, glm::vec2, int, bool> defaultValue;
    std::optional<float> min;
    std::optional<float> max;
    std::vector<int> values;           // For "long" type dropdown
    std::vector<std::string> labels;   // For "long" type dropdown
};

struct ISFResource {
    std::string name;
    std::string type;      // "storage_buffer", "image2d"
    std::string structure; // GLSL type or struct name
};

struct ISFPayloadField {
    std::string name;
    std::string type;
};

struct ISFPayload {
    std::string name;
    std::vector<ISFPayloadField> fields;
};

struct ISFShaderDefinition {
    ShaderStage stage;
    std::string description;
    std::vector<ISFInput> inputs;
    std::vector<ISFResource> resources;
    std::vector<ISFPayload> payloads;
    std::string glslSource;
};

class ISFParser {
  public:
    // Parse ISF source string
    static ISFShaderDefinition parse(const std::string &isfSource);

    // Parse ISF file
    static ISFShaderDefinition parseFile(const std::filesystem::path &path);

  private:
    static ShaderStage parseStage(const std::string &stageStr);
};

} // namespace tracey
