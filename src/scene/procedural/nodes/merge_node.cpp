#include "merge_node.hpp"
#include "../node_graph.hpp"
#include "../node_registry.hpp"

namespace tracey
{
    // Static registration (called before main())
    bool MergeNode::s_registered = MergeNode::registerNode();

    bool MergeNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_GEOMETRY_MERGE;
        desc.name = "Merge";
        desc.description = "Merge multiple geometries into one";
        desc.category = NodeCategory::Geometry;
        desc.icon = "⚙";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<MergeNode>(uid, std::move(name));
        };

        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    MergeNode::MergeNode(size_t uid, std::string name)
        : ProceduralNode(uid, NodeType::GeometryMerge, std::move(name))
    {
        initializeParameters();
    }

    void MergeNode::initializeParameters()
    {
        // Merge node doesn't have parameters - it works purely through connections
        // All incoming geometry connections are automatically merged
    }

    const InputsAndOutputs* MergeNode::ports() const
    {
        static InputsAndOutputs portInfo;
        static bool initialized = false;

        if (!initialized) {
            // Merge accepts multiple inputs (though only 2 primary ports are defined)
            portInfo.addInput(PortInfo::createInput("input0", DataType::Geometry));
            portInfo.addInput(PortInfo::createInput("input1", DataType::Geometry));
            portInfo.addOutput(PortInfo::createOutput("geometry", DataType::Geometry));
            initialized = true;
        }

        return &portInfo;
    }

    NodeEvaluationResult MergeNode::evaluate(const EvaluationContext& ctx)
    {
        NodeEvaluationResult result;

        try {
            // Find all incoming connections to this node
            std::vector<std::shared_ptr<Geometry>> inputGeometries;

            if (ctx.graph) {
                for (const auto& conn : ctx.graph->connections()) {
                    if (conn.toNode == uid()) {
                        // Get the source node's cached result
                        auto cacheIt = ctx.cache.find(conn.fromNode);
                        if (cacheIt != ctx.cache.end()) {
                            const auto& nodeResult = cacheIt->second;
                            // Try to extract Geometry from the result
                            if (auto* geomPtr = std::get_if<std::shared_ptr<Geometry>>(&nodeResult.data)) {
                                inputGeometries.push_back(*geomPtr);
                            }
                        }
                    }
                }
            }

            // If no inputs, return empty geometry
            if (inputGeometries.empty()) {
                result.data = std::make_shared<Geometry>();
                result.success = true;
                return result;
            }

            // If single input, just pass it through
            if (inputGeometries.size() == 1) {
                result.data = inputGeometries[0];
                result.success = true;
                return result;
            }

            // Merge all geometries
            // Start with a copy of the first geometry's data
            Geometry merged;
            merged.setPositions(inputGeometries[0]->positions());
            merged.setIndices(inputGeometries[0]->indices());
            merged.setNormals(inputGeometries[0]->normals());
            merged.setUvs(inputGeometries[0]->uvs());

            // Merge the rest
            for (size_t i = 1; i < inputGeometries.size(); ++i) {
                mergeGeometry(merged, *inputGeometries[i]);
            }

            result.data = std::make_shared<Geometry>(std::move(merged));
            result.success = true;

        } catch (const std::exception& e) {
            result.success = false;
            result.error = std::string("Merge node failed: ") + e.what();
        }

        return result;
    }

    void MergeNode::mergeGeometry(Geometry& a, const Geometry& b)
    {
        uint32_t vertexOffset = static_cast<uint32_t>(a.positions().size());

        // Merge positions
        std::vector<Vec3> positions = a.positions();
        const auto& bPositions = b.positions();
        positions.insert(positions.end(), bPositions.begin(), bPositions.end());
        a.setPositions(std::move(positions));

        // Merge normals if both have them
        if (a.hasNormals() && b.hasNormals()) {
            std::vector<Vec3> normals = a.normals();
            const auto& bNormals = b.normals();
            normals.insert(normals.end(), bNormals.begin(), bNormals.end());
            a.setNormals(std::move(normals));
        }

        // Merge UVs if both have them
        if (a.hasUvs() && b.hasUvs()) {
            std::vector<Vec2> uvs = a.uvs();
            const auto& bUvs = b.uvs();
            uvs.insert(uvs.end(), bUvs.begin(), bUvs.end());
            a.setUvs(std::move(uvs));
        }

        // Merge indices with offset
        if (a.hasIndices() && b.hasIndices()) {
            std::vector<uint32_t> indices = a.indices();
            const auto& bIndices = b.indices();

            // Add b's indices with offset
            for (uint32_t idx : bIndices) {
                indices.push_back(idx + vertexOffset);
            }
            a.setIndices(std::move(indices));
        }

        // TODO: Merge attributes when needed
    }

} // namespace tracey
