#include "IO/STLExporter.h"
#include "Core/Logger.h"
#include <fstream>
#include <cstring>

namespace mf {

bool STLExporter::exportScene(const Scene& scene, const std::string& filepath) {
    MeshData merged;
    const_cast<Scene&>(scene).root()->traverse([&](SceneNode* node) {
        if (node->type() != SceneNode::Type::Mesh || !node->mesh() || node->mesh()->lods.empty())
            return;
        const auto& md = node->mesh()->lods[0].mesh;
        if (md.vertices.empty()) return;

        uint32_t offset = static_cast<uint32_t>(merged.vertices.size());
        for (const auto& v : md.vertices) {
            Vertex wv = v;
            wv.position = Vec3(node->worldTransform() * Vec4(v.position, 1.0f));
            wv.normal = glm::normalize(Vec3(node->worldTransform() * Vec4(v.normal, 0.0f)));
            merged.vertices.push_back(wv);
        }
        for (auto idx : md.indices) {
            merged.indices.push_back(offset + idx);
        }
    });

    if (merged.vertices.empty()) {
        MF_WARN("No mesh data to export to STL");
        return false;
    }

    merged.computeAABB();
    return exportMesh(merged, filepath, "model");
}

bool STLExporter::exportNodes(const std::vector<SceneNode*>& nodes, const std::string& filepath) {
    if (nodes.empty()) {
        MF_WARN("No nodes selected for STL export");
        return false;
    }

    MeshData merged;
    for (auto* node : nodes) {
        if (!node || node->type() != SceneNode::Type::Mesh || !node->mesh() || node->mesh()->lods.empty())
            continue;
        const auto& md = node->mesh()->lods[0].mesh;
        if (md.vertices.empty()) continue;

        uint32_t offset = static_cast<uint32_t>(merged.vertices.size());
        for (const auto& v : md.vertices) {
            Vertex wv = v;
            wv.position = Vec3(node->worldTransform() * Vec4(v.position, 1.0f));
            wv.normal = glm::normalize(Vec3(node->worldTransform() * Vec4(v.normal, 0.0f)));
            merged.vertices.push_back(wv);
        }
        for (auto idx : md.indices) {
            merged.indices.push_back(offset + idx);
        }
    }

    if (merged.vertices.empty()) {
        MF_WARN("Selected nodes have no mesh data to export to STL");
        return false;
    }

    merged.computeAABB();
    return exportMesh(merged, filepath, "selected");
}

bool STLExporter::exportMesh(const MeshData& mesh, const std::string& filepath, const std::string& name) {
    (void)name;
    std::ofstream out(filepath, std::ios::binary);
    if (!out) {
        MF_ERROR("Failed to open STL file for writing: {}", filepath);
        return false;
    }

    // 80 byte header
    char header[80] = {};
    std::strncpy(header, "MeshForge STL Export", 80);
    out.write(header, 80);

    // Triangle count
    uint32_t triCount = mesh.triangleCount();
    out.write(reinterpret_cast<const char*>(&triCount), 4);

    for (size_t i = 0; i < triCount; ++i) {
        // Compute face normal
        Vec3 normal(0, 0, 1);
        if (i * 3 + 2 < mesh.indices.size()) {
            uint32_t i0 = mesh.indices[i * 3];
            uint32_t i1 = mesh.indices[i * 3 + 1];
            uint32_t i2 = mesh.indices[i * 3 + 2];
            if (i0 < mesh.vertices.size() && i1 < mesh.vertices.size() && i2 < mesh.vertices.size()) {
                Vec3 v0 = mesh.vertices[i0].position;
                Vec3 v1 = mesh.vertices[i1].position;
                Vec3 v2 = mesh.vertices[i2].position;
                Vec3 edge1 = v1 - v0;
                Vec3 edge2 = v2 - v0;
                Vec3 n = glm::cross(edge1, edge2);
                if (glm::length(n) > 1e-8f) {
                    normal = glm::normalize(n);
                }
            }
        }
        out.write(reinterpret_cast<const char*>(&normal.x), 12);

        // 3 vertices
        for (int j = 0; j < 3; ++j) {
            uint32_t idx = mesh.indices[i * 3 + j];
            Vec3 pos = (idx < mesh.vertices.size()) ? mesh.vertices[idx].position : Vec3(0);
            out.write(reinterpret_cast<const char*>(&pos.x), 12);
        }

        // Attribute byte count (0)
        uint16_t attr = 0;
        out.write(reinterpret_cast<const char*>(&attr), 2);
    }

    out.close();
    MF_INFO("Exported STL: {} ({} triangles, {} bytes)", filepath, triCount, 84 + triCount * 50);
    return true;
}

} // namespace mf
