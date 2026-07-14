#pragma once
#include <cstdint>
#include <cstddef>

// Minimal bgfx stub - allows compilation without real bgfx
namespace bgfx {

struct VertexBufferHandle { uint16_t idx = UINT16_MAX; bool valid() const { return idx != UINT16_MAX; } };
struct IndexBufferHandle  { uint16_t idx = UINT16_MAX; bool valid() const { return idx != UINT16_MAX; } };
struct ProgramHandle     { uint16_t idx = UINT16_MAX; bool valid() const { return idx != UINT16_MAX; } };
struct UniformHandle     { uint16_t idx = UINT16_MAX; bool valid() const { return idx != UINT16_MAX; } };
struct ShaderHandle      { uint16_t idx = UINT16_MAX; bool valid() const { return idx != UINT16_MAX; } };

struct Memory { const void* data; uint32_t size; };

inline const Memory* makeRef(const void* data, uint32_t size) {
    static Memory mem;
    mem.data = data;
    mem.size = size;
    return &mem;
}

enum class RendererType : uint8_t { Metal = 2, Vulkan = 4, Direct3D12 = 6, Count };
enum class Attrib : uint8_t { Position, Normal, TexCoord0, Tangent, Count };
enum class AttribType : uint8_t { Float = 0, Uint8 = 2 };
enum class UniformType : uint8_t { Vec4 = 2, Mat4 = 4, Count };

struct Init {
    RendererType type = RendererType::Metal;
    struct { uint32_t width; uint32_t height; uint32_t reset; } resolution{};
    struct { void* nwh; } platformData{};
};

struct VertexLayout {
    VertexLayout& begin() { m_count = 0; return *this; }
    VertexLayout& add(Attrib, uint8_t, AttribType, bool = false) { return *this; }
    VertexLayout& end() { return *this; }
    uint32_t getStride() const { return 48; }
    uint32_t m_count = 0;
};

inline bool init(const Init&) { return true; }
inline void shutdown() {}
inline void setViewClear(uint16_t, uint32_t, uint32_t, float, uint8_t) {}
inline void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
inline void setViewTransform(uint16_t, const void*, const void*) {}
inline void touch(uint16_t) {}
inline void reset(uint32_t, uint32_t, uint32_t = 0) {}
inline void frame() {}
inline void destroy(VertexBufferHandle) {}
inline void destroy(IndexBufferHandle) {}
inline void destroy(ProgramHandle) {}
inline void destroy(UniformHandle) {}

inline VertexBufferHandle createVertexBuffer(const Memory*, const VertexLayout&) { return {}; }
inline IndexBufferHandle createIndexBuffer(const Memory*, uint32_t) { return {}; }
inline UniformHandle createUniform(const char*, UniformType, uint16_t = 1) { return {}; }
inline ShaderHandle createShader(const Memory*) { return {}; }
inline ProgramHandle createProgram(ShaderHandle, ShaderHandle, bool = false) { return {}; }

inline void setTransform(const void*, uint16_t = 1) {}
inline void setVertexBuffer(uint8_t, VertexBufferHandle) {}
inline void setVertexBuffer(uint8_t, VertexBufferHandle, uint32_t, uint32_t) {}
inline void setIndexBuffer(IndexBufferHandle) {}
inline void setIndexBuffer(IndexBufferHandle, uint32_t, uint32_t) {}
inline void setState(uint64_t, uint32_t = 0) {}
inline void setUniform(UniformHandle, const void*, uint16_t = 1) {}
inline void submit(uint16_t, ProgramHandle, int32_t = 0, uint8_t = 0) {}

} // namespace bgfx

// C-style forward declarations for Renderer.h compatibility
typedef bgfx::VertexBufferHandle bgfx_vertex_layout_s;
typedef bgfx::ShaderHandle      bgfx_shader_handle_s;
typedef bgfx::ProgramHandle     bgfx_program_handle_s;
typedef bgfx::UniformHandle     bgfx_uniform_handle_s;

// BGFX constants as macros (matching real bgfx)
#define BGFX_RESET_VSYNC     0x0080
#define BGFX_CLEAR_COLOR     0x0001
#define BGFX_CLEAR_DEPTH     0x0002
#define BGFX_STATE_DEFAULT   0
#define BGFX_STATE_CULL_CCW  0x0010
#define BGFX_BUFFER_INDEX32  0x20
