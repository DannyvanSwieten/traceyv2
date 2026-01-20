#include "isf_parser.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace tracey {

namespace {

// Process #include directives recursively
std::string processIncludes(const std::string &source, const std::filesystem::path &basePath, int depth = 0) {
    // Prevent infinite recursion
    if (depth > 32) {
        throw std::runtime_error("Include depth exceeded maximum (32) - possible circular includes");
    }

    std::stringstream result;
    std::istringstream sourceStream(source);
    std::string line;

    while (std::getline(sourceStream, line)) {
        // Check for #include directive
        std::regex includePattern(R"(^\s*#include\s+\"([^\"]+)\"\s*$)");
        std::smatch match;

        if (std::regex_match(line, match, includePattern)) {
            std::string includePath = match[1].str();
            std::filesystem::path fullPath = basePath / includePath;

            // Read the included file
            std::ifstream includeFile(fullPath);
            if (!includeFile.is_open()) {
                throw std::runtime_error("Failed to open included file: " + fullPath.string());
            }

            std::stringstream includeBuffer;
            includeBuffer << includeFile.rdbuf();
            std::string includeContent = includeBuffer.str();

            // Recursively process includes in the included file
            std::string processedInclude = processIncludes(includeContent, fullPath.parent_path(), depth + 1);

            // Add the processed content
            result << "// Begin include: " << includePath << "\n";
            result << processedInclude;
            result << "// End include: " << includePath << "\n";
        } else {
            // Not an include directive, keep the line as-is
            result << line << "\n";
        }
    }

    return result.str();
}

// Simple JSON value extraction helpers
std::string extractStringValue(const std::string &json, const std::string &key) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return match[1].str();
    }
    return "";
}

float extractFloatValue(const std::string &json, const std::string &key, float defaultVal = 0.0f) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9.\\-]+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return std::stof(match[1].str());
    }
    return defaultVal;
}

int extractIntValue(const std::string &json, const std::string &key, int defaultVal = 0) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9\\-]+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return std::stoi(match[1].str());
    }
    return defaultVal;
}

bool extractBoolValue(const std::string &json, const std::string &key, bool defaultVal = false) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return match[1].str() == "true";
    }
    return defaultVal;
}

std::vector<float> extractFloatArray(const std::string &json, const std::string &key) {
    std::vector<float> result;
    std::regex pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        std::string arrayContent = match[1].str();
        std::regex numPattern("[0-9.\\-]+");
        std::sregex_iterator it(arrayContent.begin(), arrayContent.end(), numPattern);
        std::sregex_iterator end;
        while (it != end) {
            result.push_back(std::stof(it->str()));
            ++it;
        }
    }
    return result;
}

std::vector<int> extractIntArray(const std::string &json, const std::string &key) {
    std::vector<int> result;
    std::regex pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        std::string arrayContent = match[1].str();
        std::regex numPattern("[0-9\\-]+");
        std::sregex_iterator it(arrayContent.begin(), arrayContent.end(), numPattern);
        std::sregex_iterator end;
        while (it != end) {
            result.push_back(std::stoi(it->str()));
            ++it;
        }
    }
    return result;
}

std::vector<std::string> extractStringArray(const std::string &json, const std::string &key) {
    std::vector<std::string> result;
    std::regex pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        std::string arrayContent = match[1].str();
        std::regex strPattern("\"([^\"]*)\"");
        std::sregex_iterator it(arrayContent.begin(), arrayContent.end(), strPattern);
        std::sregex_iterator end;
        while (it != end) {
            result.push_back((*it)[1].str());
            ++it;
        }
    }
    return result;
}

// Extract array of JSON objects
std::vector<std::string> extractObjectArray(const std::string &json, const std::string &key) {
    std::vector<std::string> result;

    // Find the key and opening bracket
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos)
        return result;

    size_t bracketStart = json.find('[', keyPos);
    if (bracketStart == std::string::npos)
        return result;

    // Find matching closing bracket
    int depth = 1;
    size_t pos = bracketStart + 1;
    size_t objectStart = std::string::npos;

    while (pos < json.size() && depth > 0) {
        char c = json[pos];
        if (c == '{') {
            if (objectStart == std::string::npos) {
                objectStart = pos;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 1 && objectStart != std::string::npos) {
                result.push_back(json.substr(objectStart, pos - objectStart + 1));
                objectStart = std::string::npos;
            }
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
        }
        pos++;
    }

    return result;
}

ISFInput parseInput(const std::string &inputJson) {
    ISFInput input;
    input.name = extractStringValue(inputJson, "NAME");
    input.type = extractStringValue(inputJson, "TYPE");

    if (input.type == "float") {
        input.defaultValue = extractFloatValue(inputJson, "DEFAULT", 0.0f);
        float minVal = extractFloatValue(inputJson, "MIN", 0.0f);
        float maxVal = extractFloatValue(inputJson, "MAX", 1.0f);
        if (inputJson.find("\"MIN\"") != std::string::npos) {
            input.min = minVal;
        }
        if (inputJson.find("\"MAX\"") != std::string::npos) {
            input.max = maxVal;
        }
    } else if (input.type == "color") {
        auto values = extractFloatArray(inputJson, "DEFAULT");
        if (values.size() >= 4) {
            input.defaultValue = glm::vec4(values[0], values[1], values[2], values[3]);
        } else if (values.size() >= 3) {
            input.defaultValue = glm::vec4(values[0], values[1], values[2], 1.0f);
        } else {
            input.defaultValue = glm::vec4(1.0f);
        }
    } else if (input.type == "point2D") {
        auto values = extractFloatArray(inputJson, "DEFAULT");
        if (values.size() >= 2) {
            input.defaultValue = glm::vec2(values[0], values[1]);
        } else {
            input.defaultValue = glm::vec2(0.0f);
        }
    } else if (input.type == "bool") {
        input.defaultValue = extractBoolValue(inputJson, "DEFAULT", false);
    } else if (input.type == "long") {
        input.defaultValue = extractIntValue(inputJson, "DEFAULT", 0);
        input.values = extractIntArray(inputJson, "VALUES");
        input.labels = extractStringArray(inputJson, "LABELS");
    }

    return input;
}

ISFResource parseResource(const std::string &resourceJson) {
    ISFResource resource;
    resource.name = extractStringValue(resourceJson, "NAME");
    resource.type = extractStringValue(resourceJson, "TYPE");
    resource.structure = extractStringValue(resourceJson, "STRUCTURE");
    return resource;
}

ISFPayload parsePayload(const std::string &payloadJson) {
    ISFPayload payload;
    payload.name = extractStringValue(payloadJson, "NAME");

    auto fieldObjects = extractObjectArray(payloadJson, "FIELDS");
    for (const auto &fieldJson : fieldObjects) {
        ISFPayloadField field;
        field.name = extractStringValue(fieldJson, "NAME");
        field.type = extractStringValue(fieldJson, "TYPE");
        payload.fields.push_back(field);
    }

    return payload;
}

} // anonymous namespace

ShaderStage ISFParser::parseStage(const std::string &stageStr) {
    if (stageStr == "RayGeneration")
        return ShaderStage::RayGeneration;
    if (stageStr == "ClosestHit")
        return ShaderStage::ClosestHit;
    if (stageStr == "Miss")
        return ShaderStage::Miss;
    if (stageStr == "Resolve")
        return ShaderStage::Resolve;
    if (stageStr == "AnyHit")
        return ShaderStage::AnyHit;
    if (stageStr == "Intersection")
        return ShaderStage::Intersection;

    throw std::runtime_error("Unknown shader stage: " + stageStr);
}

ISFShaderDefinition ISFParser::parse(const std::string &isfSource) {
    ISFShaderDefinition def;

    // Find the JSON comment block: /*{ ... }*/
    size_t jsonStart = isfSource.find("/*{");
    if (jsonStart == std::string::npos) {
        throw std::runtime_error("ISF file must contain a /*{ ... }*/ JSON metadata block");
    }

    size_t jsonEnd = isfSource.find("}*/", jsonStart);
    if (jsonEnd == std::string::npos) {
        throw std::runtime_error("ISF JSON block not properly closed with }*/");
    }

    // Extract JSON (including the braces)
    std::string jsonBlock = isfSource.substr(jsonStart + 2, jsonEnd - jsonStart - 1);

    // Extract GLSL source (everything after the JSON block)
    def.glslSource = isfSource.substr(jsonEnd + 3);

    // Trim leading whitespace from GLSL source
    size_t firstNonSpace = def.glslSource.find_first_not_of(" \t\n\r");
    if (firstNonSpace != std::string::npos) {
        def.glslSource = def.glslSource.substr(firstNonSpace);
    }

    // Parse required STAGE field
    std::string stageStr = extractStringValue(jsonBlock, "STAGE");
    if (stageStr.empty()) {
        throw std::runtime_error("ISF file must specify a STAGE field");
    }
    def.stage = parseStage(stageStr);

    // Parse optional fields
    def.description = extractStringValue(jsonBlock, "DESCRIPTION");

    // Parse INPUTS array
    auto inputObjects = extractObjectArray(jsonBlock, "INPUTS");
    for (const auto &inputJson : inputObjects) {
        def.inputs.push_back(parseInput(inputJson));
    }

    // Parse RESOURCES array
    auto resourceObjects = extractObjectArray(jsonBlock, "RESOURCES");
    for (const auto &resourceJson : resourceObjects) {
        def.resources.push_back(parseResource(resourceJson));
    }

    // Parse PAYLOADS array
    auto payloadObjects = extractObjectArray(jsonBlock, "PAYLOADS");
    for (const auto &payloadJson : payloadObjects) {
        def.payloads.push_back(parsePayload(payloadJson));
    }

    return def;
}

ISFShaderDefinition ISFParser::parseFile(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open ISF file: " + path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Process includes relative to the shader file's directory
    std::string processedSource = processIncludes(source, path.parent_path());

    return parse(processedSource);
}

} // namespace tracey
