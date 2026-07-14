#include "Mesh/MeshData.h"
#include "Core/Logger.h"
#include <fstream>
#include <filesystem>
#include <mutex>

namespace mf {

MeshCache::MeshCache(const std::string& cacheDir) : m_cacheDir(cacheDir) {
    std::filesystem::create_directories(m_cacheDir);
}

bool MeshCache::has(const MeshCacheKey& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_memory.find(key) != m_memory.end()) return true;
    return std::filesystem::exists(keyToPath(key));
}

std::shared_ptr<MeshData> MeshCache::load(const MeshCacheKey& key) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_memory.find(key);
        if (it != m_memory.end()) {
            ++m_hits;
            return it->second;
        }
    }

    std::string path = keyToPath(key);
    if (!std::filesystem::exists(path)) {
        ++m_misses;
        return nullptr;
    }

    auto mesh = std::make_shared<MeshData>();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ++m_misses;
        return nullptr;
    }

    uint32_t vcount, icount;
    file.read(reinterpret_cast<char*>(&vcount), sizeof(vcount));
    file.read(reinterpret_cast<char*>(&icount), sizeof(icount));

    mesh->vertices.resize(vcount);
    mesh->indices.resize(icount);
    file.read(reinterpret_cast<char*>(mesh->vertices.data()), vcount * sizeof(Vertex));
    file.read(reinterpret_cast<char*>(mesh->indices.data()), icount * sizeof(Index));
    file.read(reinterpret_cast<char*>(&mesh->aabb.min), sizeof(Vec3));
    file.read(reinterpret_cast<char*>(&mesh->aabb.max), sizeof(Vec3));

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_memory[key] = mesh;
        m_lru.push_back(key);
        m_memoryUsed += vcount * sizeof(Vertex) + icount * sizeof(Index);
    }

    ++m_hits;
    return mesh;
}

void MeshCache::store(const MeshCacheKey& key, const MeshData& mesh) {
    std::string path = keyToPath(key);
    std::ofstream file(path, std::ios::binary);
    if (!file) return;

    uint32_t vcount = mesh.vertexCount();
    uint32_t icount = mesh.indexCount();
    file.write(reinterpret_cast<const char*>(&vcount), sizeof(vcount));
    file.write(reinterpret_cast<const char*>(&icount), sizeof(icount));
    file.write(reinterpret_cast<const char*>(mesh.vertices.data()), vcount * sizeof(Vertex));
    file.write(reinterpret_cast<const char*>(mesh.indices.data()), icount * sizeof(Index));
    file.write(reinterpret_cast<const char*>(&mesh.aabb.min), sizeof(Vec3));
    file.write(reinterpret_cast<const char*>(&mesh.aabb.max), sizeof(Vec3));

    auto ptr = std::make_shared<MeshData>(mesh);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory[key] = ptr;
    m_lru.push_back(key);
    m_memoryUsed += vcount * sizeof(Vertex) + icount * sizeof(Index);
}

void MeshCache::setMemoryBudget(size_t bytes) {
    m_memoryBudget = bytes;
}

void MeshCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.clear();
    m_lru.clear();
    m_memoryUsed = 0;
}

std::string MeshCache::keyToPath(const MeshCacheKey& key) const {
    std::hash<std::string> h;
    auto hash = h(key.shapeId + std::to_string(key.linearDeflection) + std::to_string(key.angularDeflection));
    return m_cacheDir + "/mesh_" + std::to_string(hash) + ".bin";
}

} // namespace mf
