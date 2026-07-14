#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>

namespace mf {

// ------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------
struct Camera {
    Vec3 position = Vec3(5, 5, 5);
    Vec3 target = Vec3(0, 0, 0);
    Vec3 up = Vec3(0, 1, 0);
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
    float aspect = 1.0f;

    Mat4 view() const { return glm::lookAt(position, target, up); }
    Mat4 projection() const {
        return glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
    }
    Mat4 viewProjection() const { return projection() * view(); }
};

// ------------------------------------------------------------------
// GPU resource handles (16-bit indices for bgfx)
// ------------------------------------------------------------------
struct GPUMesh {
    uint16_t vbh = UINT16_MAX;
    uint16_t ibh = UINT16_MAX;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    AABB aabb;
    bool valid() const { return vbh != UINT16_MAX && ibh != UINT16_MAX; }
};

struct Material {
    Vec4 baseColor = Vec4(0.8f, 0.8f, 0.8f, 1.0f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    bool wireframe = false;
    bool doubleSided = false;
};

struct DrawCall {
    GPUMesh* mesh = nullptr;
    Mat4 transform;
    MaterialHandle material;
    uint32_t depth = 0;
};

// ------------------------------------------------------------------
// Renderer backend type
// ------------------------------------------------------------------
enum class RenderBackend {
    OpenGL,   // Direct OpenGL 3.2 (existing, always available)
    bgfx      // bgfx cross-platform (Metal/Vulkan/DX12)
};

// ------------------------------------------------------------------
// Renderer interface
// ------------------------------------------------------------------
class Renderer {
public:
    Renderer();
    ~Renderer();

    // Initialize rendering backend
    bool init(RenderBackend backend, void* nativeWindow, uint32_t width, uint32_t height);
    void shutdown();

    // Begin/end frame
    void beginFrame();
    void endFrame();

    // Mesh management
    GPUMesh createMesh(const MeshData& data);
    void updateMesh(GPUMesh& mesh, const MeshData& data);
    void destroyMesh(GPUMesh& mesh);

    // Draw a mesh with transform
    void submit(const GPUMesh& mesh, const Mat4& transform, const Material& material);

    // Camera
    void setCamera(const Camera& camera);
    const Camera& camera() const { return m_camera; }

    // Viewport size
    void resize(uint32_t width, uint32_t height);

    // Stats
    uint32_t frameTriangleCount() const { return m_frameTriangles; }
    uint32_t frameDrawCalls() const { return m_frameDrawCalls; }
    RenderBackend backend() const { return m_backend; }

    // Highlight a buffer range (for selection visualization)
    void setHighlightRange(uint32_t indexOffset, uint32_t indexCount);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    RenderBackend m_backend = RenderBackend::OpenGL;
    Camera m_camera;
    uint32_t m_frameTriangles = 0;
    uint32_t m_frameDrawCalls = 0;
    uint32_t m_width = 1920, m_height = 1080;
};

// ------------------------------------------------------------------
// bgfx-specific vertex layout
// ------------------------------------------------------------------
struct bgfxVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, tw;

    static void init();
};

} // namespace mf
