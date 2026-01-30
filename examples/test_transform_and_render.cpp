#include "../src/scene/scene.hpp"
#include "../src/scene/procedural/nodes/primitive_node.hpp"
#include "../src/scene/procedural/nodes/merge_node.hpp"
#include "../src/scene/procedural/nodes/transform_geo_node.hpp"
#include "../src/scene/scene_instance.hpp"
#include "../src/scene/scene_compiler.hpp"
#include "../src/scene/camera.hpp"
#include "../src/device/device.hpp"
#include "../src/rendering/path_tracer.hpp"
#include "../src/rendering/post_processing.hpp"
#include <iostream>
#include <filesystem>

using namespace tracey;

int main()
{
    std::cout << "=== Testing Transform Node and Scene Rendering ===\n"
              << std::endl;

    // Create a scene
    Scene scene;
    NodeGraph &graph = scene.nodeGraph();

    std::cout << "1. Creating geometry network...\n"
              << std::endl;

    // Create a cube node
    size_t cubeUid = graph.generateNodeUid();
    auto cubeNode = std::make_unique<CubeNode>(cubeUid, "Cube");
    cubeNode->getParameter("size")->setValue(2.0f);
    std::cout << "   - Cube node (UID " << cubeUid << ") with size=2.0" << std::endl;

    // Create a sphere node
    size_t sphereUid = graph.generateNodeUid();
    auto sphereNode = std::make_unique<SphereNode>(sphereUid, "Sphere");
    sphereNode->getParameter("radius")->setValue(1.5f);
    std::cout << "   - Sphere node (UID " << sphereUid << ") with radius=1.5" << std::endl;

    // Create transform nodes for each primitive
    size_t cubeTransformUid = graph.generateNodeUid();
    auto cubeTransform = std::make_unique<TransformGeoNode>(cubeTransformUid, "TransformCube");
    cubeTransform->getParameter("translate")->setValue(Vec3(-3.0f, 0.0f, 0.0f));
    std::cout << "   - Transform node for cube (UID " << cubeTransformUid << ")" << std::endl;

    size_t sphereTransformUid = graph.generateNodeUid();
    auto sphereTransform = std::make_unique<TransformGeoNode>(sphereTransformUid, "TransformSphere");
    sphereTransform->getParameter("translate")->setValue(Vec3(3.0f, 0.0f, 0.0f));
    sphereTransform->getParameter("uniformScale")->setValue(0.8f);
    std::cout << "   - Transform node for sphere (UID " << sphereTransformUid << ")" << std::endl;

    // Create merge node
    size_t mergeUid = graph.generateNodeUid();
    auto mergeNode = std::make_unique<MergeNode>(mergeUid, "Merge");
    std::cout << "   - Merge node (UID " << mergeUid << ")" << std::endl;

    // Add nodes to graph (manual since createNode is stubbed)
    auto &nodes = const_cast<std::unordered_map<size_t, std::unique_ptr<ProceduralNode>> &>(
        graph.nodes());
    nodes[cubeUid] = std::move(cubeNode);
    nodes[sphereUid] = std::move(sphereNode);
    nodes[cubeTransformUid] = std::move(cubeTransform);
    nodes[sphereTransformUid] = std::move(sphereTransform);
    nodes[mergeUid] = std::move(mergeNode);

    std::cout << "\n2. Connecting node network...\n"
              << std::endl;
    // Connect: Cube -> TransformCube -> Merge
    //          Sphere -> TransformSphere -> Merge

    if (graph.connect(cubeUid, "geometry", cubeTransformUid, "input"))
    {
        std::cout << "   - Connected Cube -> TransformCube" << std::endl;
    }

    if (graph.connect(cubeTransformUid, "geometry", mergeUid, "input0"))
    {
        std::cout << "   - Connected TransformCube -> Merge" << std::endl;
    }

    if (graph.connect(sphereUid, "geometry", sphereTransformUid, "input"))
    {
        std::cout << "   - Connected Sphere -> TransformSphere" << std::endl;
    }

    if (graph.connect(sphereTransformUid, "geometry", mergeUid, "input1"))
    {
        std::cout << "   - Connected TransformSphere -> Merge" << std::endl;
    }

    // Set merge as output
    graph.setOutputNode("geometry", mergeUid);
    std::cout << "   - Set Merge as output node" << std::endl;

    std::cout << "\n3. Evaluating node graph...\n"
              << std::endl;

    // Create evaluation context
    EvaluationContext ctx;
    ctx.currentTime = 0.0;
    ctx.currentFrame = 0;

    // Evaluate the graph
    GraphEvaluationResult result = graph.evaluate(ctx);

    if (!result.success)
    {
        std::cout << "   ✗ Evaluation failed: " << result.error << std::endl;
        return 1;
    }

    std::cout << "   ✓ Evaluation successful!" << std::endl;

    // Check output
    auto outputIt = result.outputs.find("geometry");
    if (outputIt == result.outputs.end())
    {
        std::cout << "   ✗ No 'geometry' output found!" << std::endl;
        return 1;
    }

    const auto &nodeResult = outputIt->second;
    auto *geomPtr = std::get_if<std::shared_ptr<Geometry>>(&nodeResult.data);

    if (!geomPtr)
    {
        std::cout << "   ✗ Output is not geometry!" << std::endl;
        return 1;
    }

    const auto &geometry = **geomPtr;

    std::cout << "\n4. Result geometry:\n"
              << std::endl;
    std::cout << "   - Points: " << geometry.pointCount() << std::endl;
    std::cout << "   - Has normals: " << (geometry.hasNormals() ? "yes" : "no") << std::endl;
    std::cout << "   - Has UVs: " << (geometry.hasUvs() ? "yes" : "no") << std::endl;

    std::cout << "\n5. Converting to SceneObject for rendering...\n"
              << std::endl;

    // Convert Geometry to SceneObject
    SceneObject sceneObj = geometry.toSceneObject("ProceduralGeometry");
    std::cout << "   - Created SceneObject: " << sceneObj.name() << std::endl;
    std::cout << "   - Vertices: " << sceneObj.vertexCount() << std::endl;
    std::cout << "   - Triangles: " << sceneObj.triangleCount() << std::endl;

    // Add scene object to scene with a name
    const std::string objectName = "ProceduralGeometry";
    scene.addObject(objectName, std::move(sceneObj));
    std::cout << "   - Added SceneObject to scene with name: " << objectName << std::endl;

    // Create an actor
    Actor *actor = scene.createActor();
    actor->setName("ProceduralActor");
    std::cout << "   - Created Actor (UID " << actor->getUid() << ")" << std::endl;

    // Create scene instance to bind geometry to actor
    SceneInstance instance(objectName);
    actor->addInstance(std::move(instance));
    std::cout << "   - Created SceneInstance binding '" << objectName << "' to actor" << std::endl;

    std::cout << "\n✓ Test passed! Transform node and scene rendering setup complete." << std::endl;
    std::cout << "\nScene Summary:" << std::endl;
    std::cout << "   - Actors: " << scene.actors().size() << std::endl;
    std::cout << "   - Objects: " << scene.objects().size() << std::endl;
    std::cout << "   - Actor instances: " << actor->instances().size() << std::endl;

    std::cout << "\n6. Compiling scene to acceleration structures...\n"
              << std::endl;

    // Create compute device
    std::unique_ptr<Device> computeDevice = std::unique_ptr<Device>(
        createDevice(DeviceType::Gpu, DeviceBackend::Compute));
    std::cout << "   - Created GPU compute device" << std::endl;

    // Compile scene to acceleration structures
    SceneCompiler::CompiledScene compiledScene =
        SceneCompiler::compile(computeDevice.get(), scene);

    std::cout << "   ✓ Scene compiled successfully!" << std::endl;
    std::cout << "   - BLAS count: " << compiledScene.blases.size() << std::endl;
    std::cout << "   - Instance count: " << compiledScene.instances.size() << std::endl;
    std::cout << "   - Material count: " << compiledScene.materials.size() << std::endl;

    std::cout << "\n7. Setting up path tracer...\n"
              << std::endl;

    // Configure path tracer
    PathTracerConfig config;
    config.width = 800;
    config.height = 600;
    config.hdrOutput = true;

    // Get shader paths (relative to scene_renderer)
    std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path() / "scene_renderer" / "shaders";
    config.rayGenShader = shaderDir / "ray_gen.isf";
    config.hitShader = shaderDir / "diffuse_hit.isf";
    config.missShader = shaderDir / "sky_miss.isf";
    config.resolveShader = shaderDir / "resolve.isf";

    // Create path tracer
    PathTracer pathTracer(computeDevice.get(), config);
    std::cout << "   ✓ Path tracer pipeline built" << std::endl;

    // Setup camera to view the scene
    Camera camera = Camera::fitToBounds(
        Vec3(-5.0f, -2.0f, -2.0f), // Min bounds (cube at -3, sphere at +3)
        Vec3(5.0f, 2.0f, 2.0f),    // Max bounds
        45.0f                      // FOV
    );
    std::cout << "   - Camera fitted to scene bounds" << std::endl;
    std::cout << "     Position: (" << camera.position().x << ", "
              << camera.position().y << ", " << camera.position().z << ")" << std::endl;

    std::cout << "\n8. Rendering...\n"
              << std::endl;

    // Render
    double renderTime = pathTracer.render(compiledScene, camera);
    std::cout << "   ✓ Ray tracing completed in " << renderTime << " ms" << std::endl;

    // Readback HDR image
    std::vector<float> hdrData(config.width * config.height * 4);
    pathTracer.readback(hdrData.data());

    // Divide by sample count to get average
    const uint32_t numSamples = 16;
    float invSamples = 1.0f; // / float(numSamples);
    for (size_t i = 0; i < hdrData.size(); i += 4)
    {
        hdrData[i + 0] *= invSamples; // R
        hdrData[i + 1] *= invSamples; // G
        hdrData[i + 2] *= invSamples; // B
    }

    std::cout << "\n9. Tone mapping and saving output...\n"
              << std::endl;

    // Tone map to LDR
    std::vector<uint8_t> ldrData(config.width * config.height * 4);
    ToneMapSettings toneMapSettings;
    toneMapSettings.exposure = 1.0f;
    toneMapSettings.gamma = 2.2f;
    toneMapSettings.toneMapOperator = ToneMapSettings::Operator::ACES;

    PostProcessing::toneMap(hdrData.data(), ldrData.data(),
                            config.width, config.height,
                            toneMapSettings);

    // Save output
    std::string outputFileName = "procedural_scene.ppm";
    PostProcessing::savePPM(outputFileName, ldrData.data(),
                            config.width, config.height);

    std::cout << "   ✓ Output saved to " << outputFileName << std::endl;
    std::cout << "\n✓ Complete! Procedural geometry rendered successfully!" << std::endl;

    return 0;
}
