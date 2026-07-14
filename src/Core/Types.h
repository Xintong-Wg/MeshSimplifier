#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace mf {

// ------------------------------------------------------------------
// Fundamental types
// ------------------------------------------------------------------
using Vec3 = glm::vec3;
using Vec2 = glm::vec2;
using Vec4 = glm::vec4;
using Mat4 = glm::mat4;
using Mat3 = glm::mat3;
using Quat = glm::quat;
using Color = glm::vec4;

using Index = uint32_t;
using U64   = uint64_t;

// ------------------------------------------------------------------
// Vertex layout matching bgfx expectations
// ------------------------------------------------------------------
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent; // xyz = tangent, w = sign

    static void init();
};

// ------------------------------------------------------------------
// Axis-Aligned Bounding Box
// ------------------------------------------------------------------
struct AABB {
    Vec3 min = Vec3(std::numeric_limits<float>::max());
    Vec3 max = Vec3(-std::numeric_limits<float>::max());

    void expand(const Vec3& p);
    void expand(const AABB& other);
    Vec3 center() const;
    Vec3 extent() const;
    float diagonal() const;
    bool isEmpty() const;
};

// ------------------------------------------------------------------
// Handle for GPU resources
// ------------------------------------------------------------------
template<typename Tag>
struct Handle {
    uint16_t id = UINT16_MAX;
    bool valid() const { return id != UINT16_MAX; }
    bool operator==(const Handle& o) const { return id == o.id; }
};

struct MeshTag {};
struct MaterialTag {};
struct TextureTag {};
using MeshHandle     = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle  = Handle<TextureTag>;

// ------------------------------------------------------------------
// Unique ID for scene entities
// ------------------------------------------------------------------
using EntityId = uint64_t;

} // namespace mf
