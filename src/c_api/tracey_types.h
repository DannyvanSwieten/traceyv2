#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Math Types
// ============================================================================

typedef struct {
    float x, y;
} TraceyVec2;

typedef struct {
    float x, y, z;
} TraceyVec3;

typedef struct {
    float x, y, z, w;
} TraceyVec4;

typedef struct {
    float w, x, y, z;  // GLM quaternion format: w first
} TraceyQuat;

typedef struct {
    float m[16];  // Column-major 4x4 matrix (OpenGL/GLM convention)
} TraceyMat4;

// ============================================================================
// Scene Types
// ============================================================================

typedef struct {
    TraceyVec3 position;
    TraceyQuat rotation;
    TraceyVec3 scale;
} TraceyTransform;

typedef struct {
    TraceyVec3 position;
    TraceyQuat rotation;
    float fov;           // Field of view in degrees
    float nearPlane;
    float farPlane;
    float aspectRatio;
} TraceyCamera;

// Material property types
typedef enum {
    TRACEY_MATERIAL_PROP_FLOAT,
    TRACEY_MATERIAL_PROP_VEC2,
    TRACEY_MATERIAL_PROP_VEC3,
    TRACEY_MATERIAL_PROP_VEC4,
    TRACEY_MATERIAL_PROP_INT,
    TRACEY_MATERIAL_PROP_TEXTURE,  // String path
} TraceyMaterialPropertyType;

typedef struct {
    const char* name;
    TraceyMaterialPropertyType type;
    union {
        float floatValue;
        TraceyVec2 vec2Value;
        TraceyVec3 vec3Value;
        TraceyVec4 vec4Value;
        int32_t intValue;
        const char* textureValue;  // Texture path
    } value;
} TraceyMaterialProperty;

typedef struct {
    const char* shaderId;
    const TraceyMaterialProperty* properties;
    uint32_t propertyCount;
} TraceyMaterialInstance;

typedef struct {
    const char* objectRef;
    TraceyMaterialInstance material;
    bool hasLocalTransform;
    TraceyTransform localTransform;  // Only valid if hasLocalTransform is true
} TraceySceneInstance;

// ============================================================================
// Device Types
// ============================================================================

typedef enum {
    TRACEY_DEVICE_CPU = 0,
    TRACEY_DEVICE_GPU = 1
} TraceyDeviceType;

typedef enum {
    TRACEY_DEVICE_BACKEND_NONE = 0,
    TRACEY_DEVICE_BACKEND_COMPUTE = 1,
    TRACEY_DEVICE_BACKEND_RTX = 2
} TraceyDeviceBackend;

typedef enum {
    TRACEY_IMAGE_FORMAT_R8G8B8A8_UNORM,
    TRACEY_IMAGE_FORMAT_R8G8B8A8_SRGB,
    TRACEY_IMAGE_FORMAT_R32G32B32A32_SFLOAT,
    TRACEY_IMAGE_FORMAT_R32_SFLOAT
} TraceyImageFormat;

// ============================================================================
// Path Tracer Configuration
// ============================================================================

typedef struct {
    uint32_t width;
    uint32_t height;
    const char* rayGenShaderPath;
    const char* hitShaderPath;
    const char* missShaderPath;
    const char* resolveShaderPath;  // Can be NULL
    bool hdrOutput;
    uint32_t samplesPerFrame;  // Samples per render call (default 16)
    uint32_t maxBounces;       // Maximum ray bounces (default 8)
} TraceyPathTracerConfig;

// ============================================================================
// Rasterizer Configuration
// ============================================================================

typedef struct {
    uint32_t width;
    uint32_t height;
    const char* vertexShaderPath;
    const char* fragmentShaderPath;
    bool useDepthBuffer;
    bool depthTestEnable;
    bool cullBackFaces;
    bool alphaBlending;
} TraceyRasterizerConfig;

// ============================================================================
// Result Codes
// ============================================================================

typedef enum {
    TRACEY_SUCCESS = 0,
    TRACEY_ERROR_INVALID_PARAMETER = -1,
    TRACEY_ERROR_OUT_OF_MEMORY = -2,
    TRACEY_ERROR_DEVICE_CREATION_FAILED = -3,
    TRACEY_ERROR_SHADER_COMPILATION_FAILED = -4,
    TRACEY_ERROR_SCENE_COMPILATION_FAILED = -5,
    TRACEY_ERROR_FILE_NOT_FOUND = -6,
    TRACEY_ERROR_RENDERING_FAILED = -7,
    TRACEY_ERROR_NULL_POINTER = -8,
    TRACEY_ERROR_NOT_FOUND = -9,
    TRACEY_ERROR_INVALID_ARGUMENT = -10,
    TRACEY_ERROR_INVALID_STATE = -11,
    TRACEY_ERROR_PRESENTATION_FAILED = -12,
    TRACEY_ERROR_UNKNOWN = -999
} TraceyResult;

// ============================================================================
// Scene Query Types
// ============================================================================

typedef struct {
    uint64_t uid;
    const char* name;
    TraceyTransform transform;
    uint32_t childCount;
    uint32_t instanceCount;
} TraceyActorInfo;

typedef struct {
    const char* objectRef;      // Reference to mesh/scene object
    const char* shaderId;       // Material shader ID
    bool hasLocalTransform;
    TraceyTransform localTransform;
} TraceyInstanceInfo;

typedef struct {
    const char* name;
    uint32_t vertexCount;
    uint32_t triangleCount;
    bool hasIndices;
    bool hasNormals;
    bool hasUvs;
} TraceyMeshInfo;

typedef struct {
    const char* id;
    int32_t width;
    int32_t height;
    int32_t channels;
    const char* mimeType;
} TraceyTextureInfo;

// ============================================================================
// Port System Types (Phase 2)
// ============================================================================

/// Data types for port validation
typedef enum {
    TRACEY_DATA_TYPE_FLOAT = 0,
    TRACEY_DATA_TYPE_VEC2 = 1,
    TRACEY_DATA_TYPE_VEC3 = 2,
    TRACEY_DATA_TYPE_VEC4 = 3,
    TRACEY_DATA_TYPE_MAT3 = 4,
    TRACEY_DATA_TYPE_MAT4 = 5,
    TRACEY_DATA_TYPE_INT = 6,
    TRACEY_DATA_TYPE_UINT = 7,
    TRACEY_DATA_TYPE_BOOL = 8,
    TRACEY_DATA_TYPE_SAMPLER2D = 9,
    TRACEY_DATA_TYPE_GEOMETRY = 10,
    TRACEY_DATA_TYPE_DATA_TYPE = 11,
    TRACEY_DATA_TYPE_SCENE3D = 12
} TraceyDataType;

/// Port type enumeration
typedef enum {
    TRACEY_PORT_INPUT = 0,
    TRACEY_PORT_OUTPUT = 1
} TraceyPortType;

/// Port information structure
typedef struct {
    const char* name;        ///< Port name
    TraceyDataType dataType; ///< Data type of the port
    TraceyPortType portType; ///< Input or output
} TraceyPortInfo;

#ifdef __cplusplus
}
#endif
