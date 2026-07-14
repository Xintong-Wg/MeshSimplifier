#include "Mesh/MeshData.h"
#include "Core/Logger.h"
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>
#include <unordered_map>
#include <fstream>
#include <cstring>
#include <cmath>

namespace mf {

void MeshData::clear() {
    vertices.clear();
    indices.clear();
    aabb = AABB{};
}

void MeshData::computeAABB() {
    aabb = AABB{};
    for (const auto& v : vertices) {
        aabb.expand(v.position);
    }
}

void MeshData::computeNormals() {
    for (auto& v : vertices) v.normal = Vec3(0.0f);

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
        Vec3 p0 = vertices[i0].position;
        Vec3 p1 = vertices[i1].position;
        Vec3 p2 = vertices[i2].position;
        Vec3 c = glm::cross(p1 - p0, p2 - p0);
        if (glm::length2(c) < 1e-20f) continue;
        Vec3 n = glm::normalize(c);
        vertices[i0].normal += n;
        vertices[i1].normal += n;
        vertices[i2].normal += n;
    }

    for (auto& v : vertices) {
        if (glm::length2(v.normal) > 1e-20f) {
            v.normal = glm::normalize(v.normal);
        } else {
            v.normal = Vec3(0, 1, 0);
        }
    }
}

void MeshData::computeTangents() {
    std::vector<Vec3> tangents(vertices.size(), Vec3(0.0f));
    std::vector<Vec3> bitangents(vertices.size(), Vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        Vec3 p0 = vertices[i0].position;
        Vec3 p1 = vertices[i1].position;
        Vec3 p2 = vertices[i2].position;

        Vec2 uv0 = vertices[i0].uv;
        Vec2 uv1 = vertices[i1].uv;
        Vec2 uv2 = vertices[i2].uv;

        Vec3 e1 = p1 - p0;
        Vec3 e2 = p2 - p0;

        Vec2 duv1 = uv1 - uv0;
        Vec2 duv2 = uv2 - uv0;

        float denom = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(denom) < 1e-8f) continue;
        float f = 1.0f / denom;
        Vec3 t = f * (duv2.y * e1 - duv1.y * e2);
        Vec3 b = f * (-duv2.x * e1 + duv1.x * e2);

        tangents[i0] += t;
        tangents[i1] += t;
        tangents[i2] += t;
        bitangents[i0] += b;
        bitangents[i1] += b;
        bitangents[i2] += b;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        Vec3 n = vertices[i].normal;
        if (glm::length2(n) < 1e-20f) n = Vec3(0, 1, 0);
        Vec3 t = (glm::length2(tangents[i]) > 1e-20f) ? glm::normalize(tangents[i]) : Vec3(1, 0, 0);
        // Gram-Schmidt
        Vec3 ortho = t - n * glm::dot(n, t);
        t = (glm::length2(ortho) > 1e-20f) ? glm::normalize(ortho) : Vec3(1, 0, 0);
        float w = (glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f) ? -1.0f : 1.0f;
        vertices[i].tangent = Vec4(t, w);
    }
}

const LODLevel* LODMesh::pickLOD(float screenPixels) const {
    if (lods.empty()) return nullptr;
    const LODLevel* best = &lods[0];
    for (const auto& lod : lods) {
        if (screenPixels >= lod.screenSize) {
            best = &lod;
        }
    }
    return best;
}

bool MeshCacheKey::operator==(const MeshCacheKey& o) const {
    return shapeId == o.shapeId &&
           std::abs(linearDeflection - o.linearDeflection) < 1e-6f &&
           std::abs(angularDeflection - o.angularDeflection) < 1e-6f;
}

size_t MeshCacheKeyHash::operator()(const MeshCacheKey& k) const {
    size_t h1 = std::hash<std::string>{}(k.shapeId);
    size_t h2 = std::hash<float>{}(k.linearDeflection);
    size_t h3 = std::hash<float>{}(k.angularDeflection);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
}

bool MeshData::saveToFile(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint32_t vc = vertexCount();
    uint32_t ic = indexCount();
    out.write(reinterpret_cast<const char*>(&vc), 4);
    out.write(reinterpret_cast<const char*>(&ic), 4);
    out.write(reinterpret_cast<const char*>(&aabb.min), sizeof(Vec3));
    out.write(reinterpret_cast<const char*>(&aabb.max), sizeof(Vec3));
    if (vc > 0)
        out.write(reinterpret_cast<const char*>(vertices.data()), vc * sizeof(Vertex));
    if (ic > 0)
        out.write(reinterpret_cast<const char*>(indices.data()), ic * sizeof(Index));
    return out.good();
}

bool MeshData::loadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    uint32_t vc = 0, ic = 0;
    in.read(reinterpret_cast<char*>(&vc), 4);
    in.read(reinterpret_cast<char*>(&ic), 4);
    in.read(reinterpret_cast<char*>(&aabb.min), sizeof(Vec3));
    in.read(reinterpret_cast<char*>(&aabb.max), sizeof(Vec3));

    vertices.resize(vc);
    indices.resize(ic);
    if (vc > 0)
        in.read(reinterpret_cast<char*>(vertices.data()), vc * sizeof(Vertex));
    if (ic > 0)
        in.read(reinterpret_cast<char*>(indices.data()), ic * sizeof(Index));
    return in.good();
}

} // namespace mf
