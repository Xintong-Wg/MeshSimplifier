#pragma once

#include "Core/Types.h"
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>

namespace mf {

// ------------------------------------------------------------------
// GPU-ready mesh with vertex/index buffers
// ------------------------------------------------------------------
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<Index>  indices;
    AABB aabb;

    uint32_t vertexCount() const { return static_cast<uint32_t>(vertices.size()); }
    uint32_t indexCount() const { return static_cast<uint32_t>(indices.size()); }
    uint32_t triangleCount() const { return indexCount() / 3; }

    void clear();
    void computeAABB();
    void computeNormals();
    void computeTangents();

    // Binary cache I/O
    bool saveToFile(const std::string& path) const;
    bool loadFromFile(const std::string& path);
};

// ------------------------------------------------------------------
// LOD level for a mesh
// ------------------------------------------------------------------
struct LODLevel {
    uint32_t level = 0;        // 0 = highest detail
    float    screenSize = 0.0f; // pixel threshold
    MeshData mesh;
};

// ------------------------------------------------------------------
// Mesh with multiple LODs
// ------------------------------------------------------------------
struct LODMesh {
    std::string name;
    std::vector<LODLevel> lods;

    const LODLevel* pickLOD(float screenPixels) const;
};

// ------------------------------------------------------------------
// Key for mesh cache
// ------------------------------------------------------------------
struct MeshCacheKey {
    std::string shapeId;
    float linearDeflection = 0.0f;
    float angularDeflection = 0.0f;

    bool operator==(const MeshCacheKey& o) const;
};

struct MeshCacheKeyHash {
    size_t operator()(const MeshCacheKey& k) const;
};

// ------------------------------------------------------------------
// Mesh cache on disk + memory
// ------------------------------------------------------------------
class MeshCache {
public:
    explicit MeshCache(const std::string& cacheDir);

    bool has(const MeshCacheKey& key) const;
    std::shared_ptr<MeshData> load(const MeshCacheKey& key);
    void store(const MeshCacheKey& key, const MeshData& mesh);

    void setMemoryBudget(size_t bytes);
    void clear();

    size_t memoryUsed() const { return m_memoryUsed; }
    size_t hitCount() const { return m_hits; }
    size_t missCount() const { return m_misses; }

private:
    std::string m_cacheDir;
    size_t m_memoryBudget = 512 * 1024 * 1024; // 512MB default
    size_t m_memoryUsed = 0;
    size_t m_hits = 0;
    size_t m_misses = 0;

    mutable std::mutex m_mutex;
    std::unordered_map<MeshCacheKey, std::shared_ptr<MeshData>, MeshCacheKeyHash> m_memory;
    std::vector<MeshCacheKey> m_lru;

    std::string keyToPath(const MeshCacheKey& key) const;
};

} // namespace mf
