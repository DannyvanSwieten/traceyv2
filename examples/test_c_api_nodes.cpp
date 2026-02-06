#include "../src/c_api/tracey_api.h"
#include <iostream>
#include <cstring>

int main()
{
    std::cout << "=== Testing C API Node Graph Functions ===\n" << std::endl;

    // Create a scene
    TraceyScene* scene = tracey_scene_create();
    if (!scene) {
        std::cerr << "Failed to create scene: " << tracey_get_last_error() << std::endl;
        return 1;
    }
    std::cout << "✓ Scene created" << std::endl;

    // Get the node graph
    TraceyNodeGraph* graph = tracey_scene_get_node_graph(scene);
    if (!graph) {
        std::cerr << "Failed to get node graph: " << tracey_get_last_error() << std::endl;
        tracey_scene_destroy(scene);
        return 1;
    }
    std::cout << "✓ Node graph retrieved" << std::endl;

    std::cout << "\n1. Creating nodes..." << std::endl;

    // Create a cube node
    uint64_t cubeUid = tracey_node_graph_create_node(graph, TRACEY_NODE_PRIMITIVE_CUBE, "Cube");
    if (cubeUid == 0) {
        std::cerr << "Failed to create cube node: " << tracey_get_last_error() << std::endl;
        tracey_scene_destroy(scene);
        return 1;
    }
    std::cout << "   - Cube node created (UID: " << cubeUid << ")" << std::endl;

    // Set cube size parameter
    TraceyNode* cubeNode = tracey_node_graph_get_node(graph, cubeUid);
    if (cubeNode) {
        TraceyParameter* sizeParam = tracey_node_get_parameter(cubeNode, "size");
        if (sizeParam) {
            tracey_parameter_set_float(sizeParam, 2.0f);
            std::cout << "   - Set cube size to 2.0" << std::endl;
        }
    }

    // Create a sphere node
    uint64_t sphereUid = tracey_node_graph_create_node(graph, TRACEY_NODE_PRIMITIVE_SPHERE, "Sphere");
    if (sphereUid == 0) {
        std::cerr << "Failed to create sphere node: " << tracey_get_last_error() << std::endl;
        tracey_scene_destroy(scene);
        return 1;
    }
    std::cout << "   - Sphere node created (UID: " << sphereUid << ")" << std::endl;

    // Set sphere radius parameter
    TraceyNode* sphereNode = tracey_node_graph_get_node(graph, sphereUid);
    if (sphereNode) {
        TraceyParameter* radiusParam = tracey_node_get_parameter(sphereNode, "radius");
        if (radiusParam) {
            tracey_parameter_set_float(radiusParam, 1.5f);
            std::cout << "   - Set sphere radius to 1.5" << std::endl;
        }
    }

    // Create transform nodes
    uint64_t cubeTransformUid = tracey_node_graph_create_node(graph, TRACEY_NODE_GEOMETRY_TRANSFORM, "TransformCube");
    std::cout << "   - Cube transform node created (UID: " << cubeTransformUid << ")" << std::endl;

    uint64_t sphereTransformUid = tracey_node_graph_create_node(graph, TRACEY_NODE_GEOMETRY_TRANSFORM, "TransformSphere");
    std::cout << "   - Sphere transform node created (UID: " << sphereTransformUid << ")" << std::endl;

    // Set transform parameters
    TraceyNode* cubeTransformNode = tracey_node_graph_get_node(graph, cubeTransformUid);
    if (cubeTransformNode) {
        TraceyParameter* translateParam = tracey_node_get_parameter(cubeTransformNode, "translate");
        if (translateParam) {
            TraceyVec3 translate = {-3.0f, 0.0f, 0.0f};
            tracey_parameter_set_vec3(translateParam, translate);
            std::cout << "   - Set cube translation to (-3, 0, 0)" << std::endl;
        }
    }

    TraceyNode* sphereTransformNode = tracey_node_graph_get_node(graph, sphereTransformUid);
    if (sphereTransformNode) {
        TraceyParameter* translateParam = tracey_node_get_parameter(sphereTransformNode, "translate");
        if (translateParam) {
            TraceyVec3 translate = {3.0f, 0.0f, 0.0f};
            tracey_parameter_set_vec3(translateParam, translate);
            std::cout << "   - Set sphere translation to (3, 0, 0)" << std::endl;
        }

        TraceyParameter* scaleParam = tracey_node_get_parameter(sphereTransformNode, "uniformScale");
        if (scaleParam) {
            tracey_parameter_set_float(scaleParam, 0.8f);
            std::cout << "   - Set sphere uniform scale to 0.8" << std::endl;
        }
    }

    // Create merge node
    uint64_t mergeUid = tracey_node_graph_create_node(graph, TRACEY_NODE_GEOMETRY_MERGE, "Merge");
    std::cout << "   - Merge node created (UID: " << mergeUid << ")" << std::endl;

    std::cout << "\n2. Connecting nodes..." << std::endl;

    // Connect: Cube -> TransformCube -> Merge
    //          Sphere -> TransformSphere -> Merge

    TraceyResult result = tracey_node_graph_connect(graph, cubeUid, "geometry", cubeTransformUid, "input");
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Connected Cube -> TransformCube" << std::endl;
    } else {
        std::cerr << "   ✗ Failed to connect Cube -> TransformCube: " << tracey_get_last_error() << std::endl;
    }

    result = tracey_node_graph_connect(graph, cubeTransformUid, "geometry", mergeUid, "input0");
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Connected TransformCube -> Merge" << std::endl;
    } else {
        std::cerr << "   ✗ Failed to connect TransformCube -> Merge: " << tracey_get_last_error() << std::endl;
    }

    result = tracey_node_graph_connect(graph, sphereUid, "geometry", sphereTransformUid, "input");
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Connected Sphere -> TransformSphere" << std::endl;
    } else {
        std::cerr << "   ✗ Failed to connect Sphere -> TransformSphere: " << tracey_get_last_error() << std::endl;
    }

    result = tracey_node_graph_connect(graph, sphereTransformUid, "geometry", mergeUid, "input1");
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Connected TransformSphere -> Merge" << std::endl;
    } else {
        std::cerr << "   ✗ Failed to connect TransformSphere -> Merge: " << tracey_get_last_error() << std::endl;
    }

    // Set merge as output
    result = tracey_node_graph_set_output(graph, "geometry", mergeUid);
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Set Merge as output node" << std::endl;
    } else {
        std::cerr << "   ✗ Failed to set output node: " << tracey_get_last_error() << std::endl;
    }

    std::cout << "\n3. Verifying graph structure..." << std::endl;

    uint32_t nodeCount = tracey_node_graph_get_node_count(graph);
    std::cout << "   - Total nodes: " << nodeCount << std::endl;

    uint32_t connectionCount = tracey_node_graph_get_connection_count(graph);
    std::cout << "   - Total connections: " << connectionCount << std::endl;

    // List all connections
    std::cout << "\n   Connections:" << std::endl;
    for (uint32_t i = 0; i < connectionCount; ++i) {
        uint64_t fromNode, toNode;

        result = tracey_node_graph_get_connection(graph, i, &fromNode, &toNode);

        if (result == TRACEY_SUCCESS) {
            std::cout << "     " << i << ": Node " << fromNode
                      << " -> Node " << toNode << std::endl;
        }
    }

    std::cout << "\n4. Evaluating graph..." << std::endl;

    result = tracey_node_graph_evaluate(graph, 0.0, 0);
    if (result == TRACEY_SUCCESS) {
        std::cout << "   ✓ Graph evaluation successful!" << std::endl;
    } else {
        std::cerr << "   ✗ Graph evaluation failed: " << tracey_get_last_error() << std::endl;
        tracey_scene_destroy(scene);
        return 1;
    }

    std::cout << "\n✓ All C API tests passed!" << std::endl;
    std::cout << "\nSummary:" << std::endl;
    std::cout << "   - Created " << nodeCount << " nodes" << std::endl;
    std::cout << "   - Made " << connectionCount << " connections" << std::endl;
    std::cout << "   - Successfully evaluated node graph" << std::endl;

    // Cleanup
    tracey_scene_destroy(scene);
    std::cout << "\n✓ Scene destroyed" << std::endl;

    return 0;
}
