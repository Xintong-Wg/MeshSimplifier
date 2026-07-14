#include "IO/glTFExporter.h"
#include "Core/Logger.h"
#include "Mesh/MeshData.h"

#include <nlohmann/json.hpp>
#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/mesh/mesh.h>
#include <draco/point_cloud/point_cloud_builder.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>

namespace mf {

using json = nlohmann::json;

class glTFExporter::Impl {
public:
    bool exportScene(const Scene& scene, const std::string& filepath, const glTFExportOptions& options);

private:
    json buildNode(SceneNode* node, const glTFExportOptions& options);
    json buildMesh(const LODMesh* lodMesh, const glTFExportOptions& options,
                   std::vector<uint8_t>& buffer, size_t& bufOffset,
                   json& accessors, json& bufferViews);

    // Draco compression
    std::vector<uint8_t> compressDraco(const MeshData& mesh, const glTFExportOptions& options);

    // Collect instance groups for EXT_mesh_gpu_instancing
    struct InstanceGroup {
        std::string meshName;
        std::vector<Mat4> transforms;
        std::vector<SceneNode*> nodes;
    };
    std::vector<InstanceGroup> collectInstances(const Scene& scene);
};

glTFExporter::glTFExporter() : m_impl(std::make_unique<Impl>()) {}
glTFExporter::~glTFExporter() = default;

bool glTFExporter::exportScene(const Scene& scene, const std::string& filepath, const glTFExportOptions& options) {
    return m_impl->exportScene(scene, filepath, options);
}

bool glTFExporter::exportNodes(const std::vector<SceneNode*>& nodes,
                                const std::string& filepath, const glTFExportOptions& options) {
    if (nodes.empty()) return false;

    // Build a temporary scene with only the selected nodes
    Scene tempScene;
    auto root = tempScene.root();

    for (auto* node : nodes) {
        if (!node) continue;
        // Clone the node's subtree into temp scene
        auto clone = std::make_shared<SceneNode>(node->name(), node->type());
        clone->setLocalMatrix(node->worldTransform());
        if (node->mesh()) clone->setMesh(std::make_shared<LODMesh>(*node->mesh()));
        root->addChild(clone);
    }

    return m_impl->exportScene(tempScene, filepath, options);
}

// Helper: convert Mat4 to Transform for selected node export
static mf::Transform matToTransform(const mf::Mat4& mat) {
    mf::Transform t;
    t.translation = mf::Vec3{mat[3]};
    t.rotation = glm::quat_cast(mf::Mat3{mat});
    t.scale = mf::Vec3{1.0f};
    return t;
}

bool glTFExporter::Impl::exportScene(const Scene& scene, const std::string& filepath, const glTFExportOptions& options) {
    json gltf;
    gltf["asset"] = {{"version", "2.0"}, {"generator", "MeshForge 0.2.0 CAD-Aware"}};
    gltf["scene"] = 0;

    std::vector<uint8_t> buffer;
    size_t bufOffset = 0;

    json scenes = json::array();
    json sceneNodes = json::array();
    json nodes = json::array();
    json meshes = json::array();
    json materials = json::array();
    json accessors = json::array();
    json bufferViews = json::array();
    json buffers = json::array();
    std::unordered_set<std::string> extensionsUsed;
    std::unordered_set<std::string> extensionsRequired;

    // Collect instance groups if enabled
    std::vector<InstanceGroup> instanceGroups;
    if (options.exportInstances) {
        instanceGroups = collectInstances(scene);
    }
    std::unordered_set<std::string> instancedMeshNames;
    for (auto& ig : instanceGroups) instancedMeshNames.insert(ig.meshName);

    // First pass: export meshes (including instanced meshes)
    std::unordered_map<std::string, size_t> meshIndices;

    // Traverse scene for unique meshes
    const_cast<Scene&>(scene).root()->traverse([&](SceneNode* node) {
        if (node->type() == SceneNode::Type::Mesh && node->mesh()) {
            std::string meshName = node->mesh()->name;

            // Skip if already exported
            if (meshIndices.find(meshName) != meshIndices.end()) return;

            size_t meshIdx = meshes.size();
            meshIndices[meshName] = meshIdx;
            auto meshJson = buildMesh(node->mesh().get(), options, buffer, bufOffset, accessors, bufferViews);
            meshes.push_back(meshJson);
        }
    });

    // Second pass: export nodes
    const_cast<Scene&>(scene).root()->traverse([&](SceneNode* node) {
        if (node->type() == SceneNode::Type::Mesh && node->mesh()) {
            std::string meshName = node->mesh()->name;

            // Check if this mesh is instanced (skip instance nodes, they go in extension)
            if (instancedMeshNames.count(meshName)) {
                bool isFirstInstance = false;
                for (auto& ig : instanceGroups) {
                    if (ig.meshName == meshName && ig.nodes[0] == node) {
                        isFirstInstance = true;
                        break;
                    }
                }
                if (!isFirstInstance) return; // Only first instance gets a node
            }

            auto nodeJson = buildNode(node, options);
            nodeJson["mesh"] = meshIndices[meshName];

            // Add EXT_mesh_gpu_instancing for instance groups
            if (instancedMeshNames.count(meshName)) {
                for (auto& ig : instanceGroups) {
                    if (ig.meshName == meshName) {
                        json instExt;
                        json attributes;
                        json translations = json::array();

                        for (auto& t : ig.transforms) {
                            // Extract translation from matrix (column 3)
                            translations.push_back({t[3][0], t[3][1], t[3][2]});
                        }
                        attributes["TRANSLATION"] = translations;
                        instExt["attributes"] = attributes;
                        nodeJson["extensions"] = {{"EXT_mesh_gpu_instancing", instExt}};

                        extensionsUsed.insert("EXT_mesh_gpu_instancing");
                        break;
                    }
                }
            }

            nodes.push_back(nodeJson);
            sceneNodes.push_back(nodes.size() - 1);
        } else if (node->type() == SceneNode::Type::Group && options.preserveHierarchy) {
            // Export group nodes for assembly hierarchy
            auto nodeJson = buildNode(node, options);
            if (!node->children().empty()) {
                nodes.push_back(nodeJson);
            }
        }
    });

    scenes.push_back({{"nodes", sceneNodes}});

    // Track extensions
    if (options.useDraco) {
        extensionsUsed.insert("KHR_draco_mesh_compression");
    }

    gltf["scenes"] = scenes;
    gltf["nodes"] = nodes;
    gltf["meshes"] = meshes;
    gltf["materials"] = materials;
    gltf["accessors"] = accessors;
    gltf["bufferViews"] = bufferViews;

    if (!extensionsUsed.empty()) {
        json extUsedArr = json::array();
        for (auto& e : extensionsUsed) extUsedArr.push_back(e);
        gltf["extensionsUsed"] = extUsedArr;
    }

    if (!buffer.empty()) {
        buffers.push_back({{"byteLength", buffer.size()}});
        gltf["buffers"] = buffers;
    }

    if (options.embedBuffers) {
        // Write GLB
        std::string jsonStr = gltf.dump();
        size_t jsonLen = jsonStr.size();
        size_t jsonPadding = (4 - (jsonLen % 4)) % 4;
        size_t bufLen = buffer.size();
        size_t bufPadding = (4 - (bufLen % 4)) % 4;

        uint32_t totalLen = 12 + 8 + static_cast<uint32_t>(jsonLen + jsonPadding) + 8 + static_cast<uint32_t>(bufLen + bufPadding);

        std::ofstream out(filepath, std::ios::binary);
        if (!out) return false;

        // Header
        uint32_t magic = 0x46546C67; // 'glTF'
        uint32_t version = 2;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLen), 4);

        // JSON chunk
        uint32_t jsonChunkLen = static_cast<uint32_t>(jsonLen + jsonPadding);
        uint32_t jsonChunkType = 0x4E4F534A; // 'JSON'
        out.write(reinterpret_cast<const char*>(&jsonChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
        out.write(jsonStr.data(), jsonLen);
        for (size_t i = 0; i < jsonPadding; ++i) out.put(' ');

        // BIN chunk
        if (!buffer.empty()) {
            uint32_t binChunkLen = static_cast<uint32_t>(bufLen + bufPadding);
            uint32_t binChunkType = 0x004E4942; // 'BIN'
            out.write(reinterpret_cast<const char*>(&binChunkLen), 4);
            out.write(reinterpret_cast<const char*>(&binChunkType), 4);
            out.write(reinterpret_cast<const char*>(buffer.data()), bufLen);
            for (size_t i = 0; i < bufPadding; ++i) out.put('\0');
        }

        MF_INFO("Exported glb: {} ({} bytes)", filepath, totalLen);
    } else {
        // Write .gltf (JSON only, separate .bin)
        // For now just write JSON with embedded base64 buffer
        std::string jsonStr = gltf.dump(2);
        std::ofstream out(filepath);
        if (!out) return false;
        out << jsonStr;
        MF_INFO("Exported gltf: {}", filepath);
    }

    return true;
}

json glTFExporter::Impl::buildNode(SceneNode* node, const glTFExportOptions& options) {
    (void)options;
    json j;
    j["name"] = node->name();

    Mat4 m = node->worldTransform();
    std::vector<float> matData(16);
    std::memcpy(matData.data(), &m[0][0], 64);
    j["matrix"] = matData;
    return j;
}

json glTFExporter::Impl::buildMesh(const LODMesh* lodMesh, const glTFExportOptions& options,
                                   std::vector<uint8_t>& buffer, size_t& bufOffset,
                                   json& accessors, json& bufferViews) {
    json mesh;
    mesh["name"] = lodMesh->name;
    json primitives = json::array();

    if (lodMesh->lods.empty()) return mesh;
    const auto& meshData = lodMesh->lods[0].mesh;
    if (meshData.vertices.empty() || meshData.indices.empty()) return mesh;

    if (options.useDraco) {
        // Draco-compressed path
        std::vector<uint8_t> dracoBuf = compressDraco(meshData, options);
        if (!dracoBuf.empty()) {
            size_t vStart = bufOffset;
            buffer.resize(buffer.size() + dracoBuf.size());
            std::memcpy(buffer.data() + vStart, dracoBuf.data(), dracoBuf.size());
            bufOffset += dracoBuf.size();

            size_t bvIdx = bufferViews.size();
            bufferViews.push_back({
                {"buffer", 0},
                {"byteOffset", vStart},
                {"byteLength", dracoBuf.size()}
            });

            // Draco extension
            json prim;
            prim["mode"] = 4;
            prim["extensions"] = {
                {"KHR_draco_mesh_compression", {
                    {"bufferView", bvIdx},
                    {"attributes", {
                        {"POSITION", 0},
                        {"NORMAL", 1}
                    }}
                }}
            };
            prim["extensionsUsed"] = json::array({"KHR_draco_mesh_compression"});
            primitives.push_back(prim);
            mesh["primitives"] = primitives;

            MF_INFO("Draco: {} bytes compressed from {:.1f}KB",
                    dracoBuf.size(),
                    (meshData.vertices.size() * sizeof(Vertex) + meshData.indices.size() * sizeof(Index)) / 1024.0f);
            return mesh;
        }
    }

    // Uncompressed path
    size_t vStart = bufOffset;
    size_t vSize = meshData.vertices.size() * sizeof(Vertex);
    size_t iStart = vStart + vSize;
    size_t iPadding = (4 - (iStart % 4)) % 4;
    iStart += iPadding;
    size_t iSize = meshData.indices.size() * sizeof(Index);
    size_t totalSize = iStart + iSize;

    buffer.resize(buffer.size() + (totalSize - bufOffset));
    std::memcpy(buffer.data() + vStart, meshData.vertices.data(), vSize);
    std::memcpy(buffer.data() + iStart, meshData.indices.data(), iSize);
    bufOffset = totalSize;

    size_t vertBVIdx = bufferViews.size();
    bufferViews.push_back({
        {"buffer", 0}, {"byteOffset", vStart}, {"byteLength", vSize},
        {"byteStride", sizeof(Vertex)}, {"target", 34962}
    });
    size_t idxBVIdx = bufferViews.size();
    bufferViews.push_back({
        {"buffer", 0}, {"byteOffset", iStart}, {"byteLength", iSize},
        {"target", 34963}
    });

    Vec3 min = meshData.aabb.min, max = meshData.aabb.max;
    size_t posAccIdx = accessors.size();
    accessors.push_back({
        {"bufferView", vertBVIdx}, {"byteOffset", 0},
        {"componentType", 5126}, {"count", meshData.vertexCount()},
        {"type", "VEC3"}, {"min", {min.x, min.y, min.z}}, {"max", {max.x, max.y, max.z}}
    });
    size_t normAccIdx = accessors.size();
    accessors.push_back({
        {"bufferView", vertBVIdx}, {"byteOffset", offsetof(Vertex, normal)},
        {"componentType", 5126}, {"count", meshData.vertexCount()}, {"type", "VEC3"}
    });
    size_t idxAccIdx = accessors.size();
    accessors.push_back({
        {"bufferView", idxBVIdx}, {"byteOffset", 0},
        {"componentType", 5125}, {"count", meshData.indexCount()}, {"type", "SCALAR"}
    });

    json prim;
    prim["mode"] = 4;
    prim["attributes"] = {{"POSITION", posAccIdx}, {"NORMAL", normAccIdx}};
    prim["indices"] = idxAccIdx;
    primitives.push_back(prim);
    mesh["primitives"] = primitives;
    return mesh;
}

// ------------------------------------------------------------------
// Draco compression using real Draco library
// ------------------------------------------------------------------
std::vector<uint8_t> glTFExporter::Impl::compressDraco(const MeshData& mesh, const glTFExportOptions& options) {
    try {
        draco::Encoder encoder;
        encoder.SetSpeedOptions(10 - options.dracoCompressionLevel,
                                10 - options.dracoCompressionLevel);
        encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, options.dracoPositionBits);
        encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, options.dracoNormalBits);
        if (mesh.vertices[0].uv.x != 0.0f || mesh.vertices[0].uv.y != 0.0f) {
            encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, options.dracoTexCoordBits);
        }

        // Build Draco mesh
        draco::Mesh dracoMesh;
        dracoMesh.set_num_points(mesh.vertexCount());

        // Position attribute
        draco::GeometryAttribute posAttr;
        posAttr.Init(draco::GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false,
                     sizeof(Vertex), 0);
        dracoMesh.AddAttribute(posAttr, true, mesh.vertexCount());

        // Normal attribute
        draco::GeometryAttribute normAttr;
        normAttr.Init(draco::GeometryAttribute::NORMAL, nullptr, 3, draco::DT_FLOAT32, false,
                     sizeof(Vertex), offsetof(Vertex, normal));
        dracoMesh.AddAttribute(normAttr, true, mesh.vertexCount());

        // Set attribute data from vertex buffer
        for (uint32_t i = 0; i < mesh.vertexCount(); ++i) {
            dracoMesh.attribute(0)->SetAttributeValue(
                draco::AttributeValueIndex(i), &mesh.vertices[i].position);
            dracoMesh.attribute(1)->SetAttributeValue(
                draco::AttributeValueIndex(i), &mesh.vertices[i].normal);
        }

        // Faces
        for (uint32_t t = 0; t < mesh.triangleCount(); ++t) {
            draco::Mesh::Face face;
            face[0] = draco::PointIndex(mesh.indices[t*3]);
            face[1] = draco::PointIndex(mesh.indices[t*3+1]);
            face[2] = draco::PointIndex(mesh.indices[t*3+2]);
            dracoMesh.AddFace(face);
        }

        draco::EncoderBuffer encBuf;
        if (encoder.EncodeMeshToBuffer(dracoMesh, &encBuf).ok()) {
            return std::vector<uint8_t>(encBuf.data(), encBuf.data() + encBuf.size());
        }
    } catch (const std::exception& e) {
        MF_WARN("Draco compression failed: {}", e.what());
    }

    return {};
}

// ------------------------------------------------------------------
// Collect instance groups for EXT_mesh_gpu_instancing
// ------------------------------------------------------------------
std::vector<glTFExporter::Impl::InstanceGroup>
glTFExporter::Impl::collectInstances(const Scene& scene) {
    std::unordered_map<std::string, InstanceGroup> groups;

    const_cast<Scene&>(scene).root()->traverse([&](SceneNode* node) {
        if (node->type() != SceneNode::Type::Mesh || !node->mesh()) return;

        auto& group = groups[node->mesh()->name];
        group.meshName = node->mesh()->name;
        group.transforms.push_back(node->worldTransform());
        group.nodes.push_back(node);
    });

    std::vector<InstanceGroup> result;
    for (auto& [k, v] : groups) {
        if (v.transforms.size() >= 2) {
            result.push_back(std::move(v));
        }
    }
    return result;
}

} // namespace mf
