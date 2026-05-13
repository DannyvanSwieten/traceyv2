#include "path_tracer.hpp"

#include <stdexcept>

namespace tracey
{
    PathTracer::PathTracer(Device *device, const PathTracerConfig &config)
        : m_device(device), m_config(config), m_shaderInputsLayout("ShaderInputs")
    {
        if (!m_device)
        {
            throw std::runtime_error("PathTracer: device cannot be null");
        }

        createOutputImage();
        createShaderInputs();

        m_backend = selectPathTracerBackend(m_device);

        PathTracerBackend::InitParams params;
        params.device = m_device;
        params.config = &m_config;
        params.outputImage = m_outputImage.get();
        params.accumulatorImage = m_accumulatorImage.get();
        params.readbackBuffer = m_readbackBuffer.get();
        params.shaderInputs = m_shaderInputs.get();
        params.shaderInputsLayout = &m_shaderInputsLayout;
        m_backend->initialize(params);
    }

    PathTracer::~PathTracer() = default;

    void PathTracer::createOutputImage()
    {
        ImageFormat format = m_config.hdrOutput
                                 ? ImageFormat::R32G32B32A32Sfloat
                                 : ImageFormat::R8G8B8A8Unorm;

        m_outputImage.reset(m_device->createImage2D(m_config.width, m_config.height, format));

        // Linear HDR accumulator: lives across frames so the running mean is
        // numerically stable (resolve reads/writes this, only tonemaps into
        // outputImage for display).
        m_accumulatorImage.reset(m_device->createImage2D(m_config.width, m_config.height,
                                                        ImageFormat::R32G32B32A32Sfloat));

        size_t pixelSize = m_config.hdrOutput ? 16 : 4;
        size_t bufferSize = m_config.width * m_config.height * pixelSize;
        m_readbackBuffer.reset(m_device->createBuffer(bufferSize, BufferUsage::TransferDst));
    }

    void PathTracer::createShaderInputs()
    {
        // Global render state (camera + render settings). Shaders see this as
        // `shaderInputs.*`. Per-material data does NOT live here -- that's in
        // the MaterialProgram parameter buffer.
        m_shaderInputsLayout.addMember({"fov",            "float", 0, false, 0});
        m_shaderInputsLayout.addMember({"cameraPosition", "vec3",  0, false, 0});
        m_shaderInputsLayout.addMember({"cameraForward",  "vec3",  0, false, 0});
        m_shaderInputsLayout.addMember({"cameraRight",    "vec3",  0, false, 0});
        m_shaderInputsLayout.addMember({"cameraUp",       "vec3",  0, false, 0});
        m_shaderInputsLayout.addMember({"maxDepth",       "uint",  0, false, 0});
        m_shaderInputsLayout.addMember({"currentSample",  "int",   0, false, 0});
        // Direct-light count for the NEE loop in the hit shader. The lights
        // SSBO is always bound; this gates iteration so lightCount = 0 means
        // "skip NEE entirely".
        m_shaderInputsLayout.addMember({"lightCount",     "uint",  0, false, 0});

        m_shaderInputs = std::make_unique<ShaderInputsBuffer>(m_device, m_shaderInputsLayout);
    }

    void PathTracer::updateCameraUniforms(const Camera &camera, uint32_t lightCount)
    {
        m_shaderInputs->setFloat("fov", camera.fov());
        m_shaderInputs->setVec3("cameraPosition", camera.position());
        m_shaderInputs->setVec3("cameraForward", camera.forward());
        m_shaderInputs->setVec3("cameraRight", camera.right());
        m_shaderInputs->setVec3("cameraUp", camera.up());
        m_shaderInputs->setInt("currentSample", m_sampleCount + 1);
        m_shaderInputs->setUint("maxDepth", m_config.maxBounces);
        m_shaderInputs->setUint("lightCount", lightCount);
        m_shaderInputs->upload();
    }

    double PathTracer::render(const SceneCompiler::CompiledScene &scene,
                              const Camera &camera,
                              bool clearAccumulation,
                              bool wantReadback)
    {
        if (clearAccumulation)
        {
            m_sampleCount = 0;
        }

        updateCameraUniforms(camera, scene.lightCount);

        const double t = m_backend->dispatch(scene, m_sampleCount, clearAccumulation, wantReadback);

        m_sampleCount++;
        return t;
    }

    size_t PathTracer::readback(void *outData)
    {
        size_t pixelSize = m_config.hdrOutput ? 16 : 4;
        size_t bufferSize = m_config.width * m_config.height * pixelSize;

        const void *gpuData = m_readbackBuffer->mapForReading();
        std::memcpy(outData, gpuData, bufferSize);
        m_readbackBuffer->unmap();

        return bufferSize;
    }

    void PathTracer::setMaterialPrograms(const MaterialProgramBuffer &programs)
    {
        if (!m_config.useMaterialPrograms)
        {
            throw std::runtime_error("PathTracer::setMaterialPrograms requires config.useMaterialPrograms");
        }
        m_currentPrograms = programs;
        m_backend->uploadMaterialPrograms(m_currentPrograms);
        m_sampleCount = 0;  // accumulator becomes invalid when programs change
    }

    void PathTracer::setMaterialParameter(uint32_t programId, uint32_t paramIdx, const Vec4 &value)
    {
        if (!m_config.useMaterialPrograms)
        {
            throw std::runtime_error("PathTracer::setMaterialParameter requires config.useMaterialPrograms");
        }
        if (programId >= m_currentPrograms.headers().size())
        {
            throw std::runtime_error("PathTracer::setMaterialParameter: programId out of range");
        }
        const auto &hdr = m_currentPrograms.headers()[programId];
        if (paramIdx >= hdr.paramCount)
        {
            throw std::runtime_error("PathTracer::setMaterialParameter: paramIdx out of range");
        }
        m_currentPrograms.parameters()[hdr.paramOffset + paramIdx] = value;
        m_backend->uploadMaterialParameters(m_currentPrograms);
        m_sampleCount = 0;  // accumulator invalidated by visible material change
    }
} // namespace tracey
