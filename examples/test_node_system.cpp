#include "../src/scene/scene.hpp"
#include "../src/scene/procedural/nodes/primitive_node.hpp"
#include "../src/scene/procedural/nodes/merge_node.hpp"
#include <iostream>

using namespace tracey;

int main() {
    std::cout << "=== Testing Procedural Node System ===" << std::endl;

    // Create a scene with node graph
    Scene scene;
    NodeGraph& graph = scene.nodeGraph();

    std::cout << "\n1. Creating primitive nodes..." << std::endl;

    // Create a cube node
    size_t cubeUid = graph.generateNodeUid();
    auto cubeNode = std::make_unique<CubeNode>(cubeUid, "MyCube");
    cubeNode->getParameter("size")->setValue(2.0f);
    std::cout << "   - Cube node (UID " << cubeUid << ") with size=2.0" << std::endl;

    // Create a sphere node
    size_t sphereUid = graph.generateNodeUid();
    auto sphereNode = std::make_unique<SphereNode>(sphereUid, "MySphere");
    sphereNode->getParameter("radius")->setValue(1.5f);
    std::cout << "   - Sphere node (UID " << sphereUid << ") with radius=1.5" << std::endl;

    // Create a merge node
    size_t mergeUid = graph.generateNodeUid();
    auto mergeNode = std::make_unique<MergeNode>(mergeUid, "MyMerge");
    std::cout << "   - Merge node (UID " << mergeUid << ")" << std::endl;

    // Add nodes to graph (need to do this manually since createNode is stubbed)
    auto& nodes = const_cast<std::unordered_map<size_t, std::unique_ptr<ProceduralNode>>&>(
        graph.nodes()
    );
    nodes[cubeUid] = std::move(cubeNode);
    nodes[sphereUid] = std::move(sphereNode);
    nodes[mergeUid] = std::move(mergeNode);

    std::cout << "\n2. Connecting nodes..." << std::endl;
    // Connect cube -> merge
    if (graph.connect(cubeUid, "geometry", mergeUid, "input0")) {
        std::cout << "   - Connected Cube -> Merge" << std::endl;
    }

    // Connect sphere -> merge
    if (graph.connect(sphereUid, "geometry", mergeUid, "input1")) {
        std::cout << "   - Connected Sphere -> Merge" << std::endl;
    }

    // Set merge as output
    graph.setOutputNode("geometry", mergeUid);
    std::cout << "   - Set Merge as output node" << std::endl;

    std::cout << "\n3. Evaluating node graph..." << std::endl;

    // Create evaluation context
    EvaluationContext ctx;
    ctx.currentTime = 0.0;
    ctx.currentFrame = 0;

    // Evaluate the graph
    GraphEvaluationResult result = graph.evaluate(ctx);

    if (result.success) {
        std::cout << "   ✓ Evaluation successful!" << std::endl;

        // Check output
        auto outputIt = result.outputs.find("geometry");
        if (outputIt != result.outputs.end()) {
            const auto& nodeResult = outputIt->second;

            if (auto* geomPtr = std::get_if<std::shared_ptr<Geometry>>(&nodeResult.data)) {
                const auto& geometry = **geomPtr;

                std::cout << "\n4. Result geometry:" << std::endl;
                std::cout << "   - Points: " << geometry.pointCount() << std::endl;
                std::cout << "   - Primitives: " << geometry.primitiveCount() << std::endl;
                std::cout << "   - Has normals: " << (geometry.hasNormals() ? "yes" : "no") << std::endl;
                std::cout << "   - Has UVs: " << (geometry.hasUvs() ? "yes" : "no") << std::endl;

                std::cout << "\n✓ Test passed! Node system is working correctly." << std::endl;
                return 0;
            } else {
                std::cout << "   ✗ Output is not geometry!" << std::endl;
                return 1;
            }
        } else {
            std::cout << "   ✗ No 'geometry' output found!" << std::endl;
            return 1;
        }
    } else {
        std::cout << "   ✗ Evaluation failed: " << result.error << std::endl;
        return 1;
    }
}
