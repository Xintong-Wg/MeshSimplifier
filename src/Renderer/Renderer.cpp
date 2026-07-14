#include "Renderer/Renderer.h"
#include "Core/Logger.h"

#ifdef MF_HAS_BGFX
#include <bgfx/bgfx.h>
#include <bx/math.h>
#endif

#include <cstring>
#include <algorithm>

namespace mf {

void bgfxVertex::init() {
    // bgfx vertex layout declaration (registered once at startup)
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class Renderer::Impl {
public:
    RenderBackend backend = RenderBackend::OpenGL;

#ifdef MF_HAS_BGFX
    bgfx::VertexLayout bgfxLayout;
    bgfx::ProgramHandle defaultProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_modelViewProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lightDir = BGFX_INVALID_HANDLE;
    bool bgfxInitialized = false;
#endif

    bool initGL(void* nativeWindow, uint32_t w, uint32_t h);
    bool initBgfx(void* nativeWindow, uint32_t w, uint32_t h);
    void shutdownBgfx();

    GPUMesh createMeshGL(const MeshData& data);
    GPUMesh createMeshBgfx(const MeshData& data);
    void destroyMeshGL(GPUMesh& mesh);
    void destroyMeshBgfx(GPUMesh& mesh);
    void submitBgfx(const GPUMesh& mesh, const Mat4& transform, const Material& mat,
                     const Camera& camera);

    uint32_t frameTriangles = 0;
    uint32_t frameDrawCalls = 0;
};

Renderer::Renderer() : m_impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(RenderBackend backend, void* nativeWindow, uint32_t width, uint32_t height) {
    m_backend = backend;
    m_width = width;
    m_height = height;
    m_impl->backend = backend;

    if (backend == RenderBackend::bgfx) {
#ifdef MF_HAS_BGFX
        return m_impl->initBgfx(nativeWindow, width, height);
#else
        MF_WARN("bgfx not available, falling back to OpenGL");
        m_backend = RenderBackend::OpenGL;
        m_impl->backend = RenderBackend::OpenGL;
#endif
    }

    return m_impl->initGL(nativeWindow, width, height);
}

void Renderer::shutdown() {
#ifdef MF_HAS_BGFX
    if (m_impl->bgfxInitialized) m_impl->shutdownBgfx();
#endif
}

void Renderer::beginFrame() {
    m_frameTriangles = 0;
    m_frameDrawCalls = 0;
#ifdef MF_HAS_BGFX
    if (m_backend == RenderBackend::bgfx && m_impl->bgfxInitialized) {
        bgfx::touch(0);
    }
#endif
}

void Renderer::endFrame() {
#ifdef MF_HAS_BGFX
    if (m_backend == RenderBackend::bgfx && m_impl->bgfxInitialized) {
        bgfx::frame();
    }
#endif
}

GPUMesh Renderer::createMesh(const MeshData& data) {
    if (m_backend == RenderBackend::bgfx) {
#ifdef MF_HAS_BGFX
        return m_impl->createMeshBgfx(data);
#endif
    }
    return m_impl->createMeshGL(data);
}

void Renderer::updateMesh(GPUMesh& mesh, const MeshData& data) {
    destroyMesh(mesh);
    mesh = createMesh(data);
}

void Renderer::destroyMesh(GPUMesh& mesh) {
    if (m_backend == RenderBackend::bgfx) {
#ifdef MF_HAS_BGFX
        m_impl->destroyMeshBgfx(mesh);
#endif
    } else {
        m_impl->destroyMeshGL(mesh);
    }
}

void Renderer::submit(const GPUMesh& mesh, const Mat4& transform, const Material& material) {
    if (!mesh.valid()) return;
    ++m_impl->frameDrawCalls;
    m_impl->frameTriangles += mesh.indexCount / 3;

#ifdef MF_HAS_BGFX
    if (m_backend == RenderBackend::bgfx && m_impl->bgfxInitialized) {
        m_impl->submitBgfx(mesh, transform, material, m_camera);
        return;
    }
#endif
    // OpenGL rendering is handled by ViewportPanel directly
    (void)transform; (void)material;
}

void Renderer::setCamera(const Camera& camera) {
    m_camera = camera;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    m_width = width; m_height = height;
    m_camera.aspect = static_cast<float>(width) / std::max(1.0f, static_cast<float>(height));
#ifdef MF_HAS_BGFX
    if (m_backend == RenderBackend::bgfx && m_impl->bgfxInitialized) {
        bgfx::reset(width, height, BGFX_RESET_VSYNC);
    }
#endif
}

void Renderer::setHighlightRange(uint32_t, uint32_t) {
    // Highlight is handled by ViewportPanel with OpenGL
}

// ------------------------------------------------------------------
// OpenGL backend (placeholder — actual GL rendering in ViewportPanel)
// ------------------------------------------------------------------
bool Renderer::Impl::initGL(void*, uint32_t, uint32_t) {
    MF_INFO("Renderer: OpenGL backend active");
    return true;
}

GPUMesh Renderer::Impl::createMeshGL(const MeshData&) {
    GPUMesh m;
    m.vbh = 1; m.ibh = 1; // valid placeholder
    return m;
}

void Renderer::Impl::destroyMeshGL(GPUMesh& m) {
    m.vbh = UINT16_MAX;
    m.ibh = UINT16_MAX;
}

// ------------------------------------------------------------------
// bgfx backend
// ------------------------------------------------------------------
#ifdef MF_HAS_BGFX

bool Renderer::Impl::initBgfx(void* nativeWindow, uint32_t w, uint32_t h) {
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;  // Apple Silicon priority
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData.nwh = nativeWindow;
    init.resolution.width = w;
    init.resolution.height = h;
    init.resolution.reset = BGFX_RESET_VSYNC;

    if (!bgfx::init(init)) {
        MF_ERROR("bgfx init failed");
        return false;
    }

    bgfx::setDebug(BGFX_DEBUG_TEXT);

    // Set view 0 clear state
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h));

    // Vertex layout
    bgfxLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float)
        .end();

    // Uniforms
    u_modelViewProj = bgfx::createUniform("u_modelViewProj", bgfx::UniformType::Mat4);
    u_lightDir = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);

    // Load default shader program from embedded shaders
    // (shader binaries would be compiled offline via shaderc)
    bgfx::ShaderHandle vsh = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;

    // For now: program will be created when shaders are available
    if (bgfx::isValid(vsh) && bgfx::isValid(fsh)) {
        defaultProgram = bgfx::createProgram(vsh, fsh, true);
    }

    bgfxInitialized = true;
    MF_INFO("Renderer: bgfx/Metal backend initialized ({}x{})", w, h);
    return true;
}

void Renderer::Impl::shutdownBgfx() {
    if (bgfx::isValid(defaultProgram)) bgfx::destroy(defaultProgram);
    if (bgfx::isValid(u_modelViewProj)) bgfx::destroy(u_modelViewProj);
    if (bgfx::isValid(u_lightDir)) bgfx::destroy(u_lightDir);
    bgfx::shutdown();
    bgfxInitialized = false;
}

GPUMesh Renderer::Impl::createMeshBgfx(const MeshData& data) {
    GPUMesh gm;
    gm.vertexCount = data.vertexCount();
    gm.indexCount = data.indexCount();
    gm.aabb = data.aabb;

    if (gm.vertexCount == 0 || gm.indexCount == 0) return gm;

    // Convert vertices to bgfx layout
    std::vector<bgfxVertex> bgfxVerts(data.vertexCount());
    for (uint32_t i = 0; i < data.vertexCount(); ++i) {
        bgfxVerts[i].x = data.vertices[i].position.x;
        bgfxVerts[i].y = data.vertices[i].position.y;
        bgfxVerts[i].z = data.vertices[i].position.z;
        bgfxVerts[i].nx = data.vertices[i].normal.x;
        bgfxVerts[i].ny = data.vertices[i].normal.y;
        bgfxVerts[i].nz = data.vertices[i].normal.z;
        bgfxVerts[i].u = data.vertices[i].uv.x;
        bgfxVerts[i].v = data.vertices[i].uv.y;
        bgfxVerts[i].tx = data.vertices[i].tangent.x;
        bgfxVerts[i].ty = data.vertices[i].tangent.y;
        bgfxVerts[i].tz = data.vertices[i].tangent.z;
        bgfxVerts[i].tw = data.vertices[i].tangent.w;
    }

    const bgfx::Memory* vbMem = bgfx::copy(bgfxVerts.data(),
        static_cast<uint32_t>(bgfxVerts.size() * sizeof(bgfxVertex)));
    gm.vbh = bgfx::createVertexBuffer(vbMem, bgfxLayout).idx;

    const bgfx::Memory* ibMem = bgfx::copy(data.indices.data(),
        static_cast<uint32_t>(data.indices.size() * sizeof(Index)));
    gm.ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32).idx;

    return gm;
}

void Renderer::Impl::destroyMeshBgfx(GPUMesh& mesh) {
    if (mesh.vbh != UINT16_MAX) {
        bgfx::destroy(bgfx::VertexBufferHandle{mesh.vbh});
        mesh.vbh = UINT16_MAX;
    }
    if (mesh.ibh != UINT16_MAX) {
        bgfx::destroy(bgfx::IndexBufferHandle{mesh.ibh});
        mesh.ibh = UINT16_MAX;
    }
}

void Renderer::Impl::submitBgfx(const GPUMesh& mesh, const Mat4& transform,
                                  const Material& mat, const Camera& camera) {
    if (!bgfx::isValid(defaultProgram)) return;

    // Model-view-projection matrix
    Mat4 mvp = camera.viewProjection() * transform;
    bgfx::setTransform(glm::value_ptr(transform));

    // Set MVP uniform
    float mvpData[16];
    std::memcpy(mvpData, glm::value_ptr(mvp), sizeof(mvpData));
    bgfx::setUniform(u_modelViewProj, mvpData);

    // Light direction
    float lightDir[4] = {0.577f, 0.577f, 0.577f, 0.0f};
    bgfx::setUniform(u_lightDir, lightDir);

    // Vertex and index buffers
    bgfx::setVertexBuffer(0, bgfx::VertexBufferHandle{mesh.vbh});
    bgfx::setIndexBuffer(bgfx::IndexBufferHandle{mesh.ibh});

    // Render state
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                     BGFX_STATE_MSAA;
    if (mat.doubleSided) {
        state |= BGFX_STATE_CULL_CCW;
    } else {
        state &= ~BGFX_STATE_CULL_CCW;
    }
    bgfx::setState(state);

    bgfx::submit(0, defaultProgram);
}

#endif // MF_HAS_BGFX

} // namespace mf
