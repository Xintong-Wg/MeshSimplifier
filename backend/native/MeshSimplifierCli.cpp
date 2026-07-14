#include "Core/Logger.h"
#include "Geometry/BRepEngine.h"
#include "Geometry/STEPReader.h"
#include "IO/STLExporter.h"
#include "IO/glTFExporter.h"
#include "Mesh/MeshData.h"
#include "Mesh/Simplifier.h"
#include "Scene/SceneGraph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Options {
    std::string input;
    std::string output;
    std::string outputDir;
    std::string metadata;
    std::string cacheDir;
    std::string format = "glb";
    std::vector<std::string> excludeNodeIds;
    std::vector<std::string> includeNodeIds;
    float ratio = 0.35f;
    float targetError = 5e-2f;
    float linearDeflection = 0.5f;
    float angularDeflection = 0.5f;
    std::optional<float> targetFileSizeMB;
    bool draco = false;
    bool selfTest = false;
    bool inspect = false;
    bool batchParts = false;
    bool help = false;
};

struct ShapeStats {
    std::string key;
    uint32_t verticesBefore = 0;
    uint32_t trianglesBefore = 0;
    uint32_t verticesAfter = 0;
    uint32_t trianglesAfter = 0;
    float achievedRatio = 1.0f;
};

struct ConversionResult {
    std::shared_ptr<mf::STEPResult> step;
    mf::Scene scene;
    std::vector<ShapeStats> stats;
    uint64_t totalVerticesBefore = 0;
    uint64_t totalTrianglesBefore = 0;
    uint64_t totalVerticesAfter = 0;
    uint64_t totalTrianglesAfter = 0;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::time_point tParse;
};

void printHelp() {
    std::cout
        << "MeshSimplifierCli\n"
        << "Usage:\n"
        << "  MeshSimplifierCli --input model.step --output model.glb [options]\n\n"
        << "Options:\n"
        << "  --format <glb|stl>          Export format, default inferred from --output or glb\n"
        << "  --batch-parts               Export each top-level part/sub-assembly into separate STL files\n"
        << "  --output-dir <dir>          Output directory for --batch-parts\n"
        << "  --ratio <0.01-1.0>          Target triangle ratio, default 0.35\n"
        << "  --target-mb <MB>            Estimate ratio from target GLB size\n"
        << "  --error <value>             Simplification error, default 0.05\n"
        << "  --linear-deflection <mm>    Tessellation deflection, default 0.5\n"
        << "  --angular-deflection <rad>  Tessellation angle, default 0.5\n"
        << "  --metadata <path>           Write conversion metadata JSON\n"
        << "  --cache-dir <path>          Optional STEP cache directory\n"
        << "  --exclude-node <id>         Exclude assembly/part node from conversion/export; can repeat\n"
        << "  --exclude-nodes-file <path> Exclude node ids listed one per line; supports id: prefixes\n"
        << "  --include-node <id>         Keep only this assembly/part node and descendants; can repeat\n"
        << "  --include-nodes-file <path> Keep node ids listed one per line; supports id: prefixes\n"
        << "  --draco                     Enable Draco compression when available\n"
        << "  --inspect                   Parse CAD assembly tree only; no tessellation/export\n"
        << "  --self-test                 Export a tiny built-in GLB for smoke testing\n"
        << "  --help                      Show this help\n";
}

float parseFloatArg(const char* value, const std::string& flag) {
    try {
        return std::stof(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid numeric value for " + flag + ": " + value);
    }
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string inferFormat(const std::string& output) {
    const auto ext = toLower(fs::path(output).extension().string());
    if (ext == ".stl") return "stl";
    return "glb";
}

std::string sanitizeFileStem(std::string value) {
    for (auto& c : value) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '_' && c != '-' && c != '.') c = '_';
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
    return value.empty() ? "part" : value;
}

void appendSplitValues(std::vector<std::string>& values, const std::string& raw) {
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t comma = raw.find(',', start);
        std::string item = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto first = item.find_first_not_of(" \t\r\n");
        auto last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) values.push_back(item.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

std::string trimToken(const std::string& raw) {
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = raw.find_last_not_of(" \t\r\n");
    return raw.substr(first, last - first + 1);
}

void appendLineValues(std::vector<std::string>& values, const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Could not read excluded nodes file: " + path);

    std::string line;
    while (std::getline(input, line)) {
        const std::string item = trimToken(line);
        if (!item.empty()) values.push_back(item);
    }
}

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto requireValue = [&](const std::string& flag) -> const char* {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + flag);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") opts.help = true;
        else if (arg == "--self-test") opts.selfTest = true;
        else if (arg == "--inspect") opts.inspect = true;
        else if (arg == "--batch-parts") opts.batchParts = true;
        else if (arg == "--input" || arg == "-i") opts.input = requireValue(arg);
        else if (arg == "--output" || arg == "-o") opts.output = requireValue(arg);
        else if (arg == "--output-dir") opts.outputDir = requireValue(arg);
        else if (arg == "--metadata") opts.metadata = requireValue(arg);
        else if (arg == "--cache-dir") opts.cacheDir = requireValue(arg);
        else if (arg == "--exclude-node") opts.excludeNodeIds.push_back(requireValue(arg));
        else if (arg == "--exclude-nodes") appendSplitValues(opts.excludeNodeIds, requireValue(arg));
        else if (arg == "--exclude-nodes-file") appendLineValues(opts.excludeNodeIds, requireValue(arg));
        else if (arg == "--include-node") opts.includeNodeIds.push_back(requireValue(arg));
        else if (arg == "--include-nodes") appendSplitValues(opts.includeNodeIds, requireValue(arg));
        else if (arg == "--include-nodes-file") appendLineValues(opts.includeNodeIds, requireValue(arg));
        else if (arg == "--format") opts.format = toLower(requireValue(arg));
        else if (arg == "--ratio") opts.ratio = parseFloatArg(requireValue(arg), arg);
        else if (arg == "--target-mb") opts.targetFileSizeMB = parseFloatArg(requireValue(arg), arg);
        else if (arg == "--error") opts.targetError = parseFloatArg(requireValue(arg), arg);
        else if (arg == "--linear-deflection") opts.linearDeflection = parseFloatArg(requireValue(arg), arg);
        else if (arg == "--angular-deflection") opts.angularDeflection = parseFloatArg(requireValue(arg), arg);
        else if (arg == "--draco") opts.draco = true;
        else throw std::runtime_error("Unknown argument: " + arg);
    }

    opts.ratio = std::clamp(opts.ratio, 0.01f, 1.0f);
    opts.targetError = std::max(0.0f, opts.targetError);
    opts.linearDeflection = std::max(0.001f, opts.linearDeflection);
    opts.angularDeflection = std::max(0.001f, opts.angularDeflection);
    if (opts.format.empty() || opts.format == "auto") opts.format = opts.output.empty() ? "glb" : inferFormat(opts.output);
    if (opts.format != "glb" && opts.format != "stl") {
        throw std::runtime_error("Unsupported --format: " + opts.format + ". Use glb or stl.");
    }
    return opts;
}

bool nodeMatchesAnyToken(const mf::AssemblyNodePtr& node, const std::unordered_set<std::string>& tokens) {
    if (!node || tokens.empty()) return false;
    const auto hasToken = [&tokens](const std::string& token) {
        return !token.empty() && tokens.contains(token);
    };
    return (!node->id.empty() && tokens.contains(node->id))
        || (!node->id.empty() && tokens.contains("id:" + node->id))
        || (!node->stepId.empty() && hasToken(node->stepId))
        || (!node->stepId.empty() && hasToken("id:" + node->stepId));
}

mf::AssemblyNodePtr cloneFilteredAssembly(
    const mf::AssemblyNodePtr& node,
    const std::unordered_set<std::string>& excludedTokens,
    bool ancestorExcluded = false) {
    if (!node) return nullptr;
    const bool excluded = ancestorExcluded || nodeMatchesAnyToken(node, excludedTokens);
    if (excluded) return nullptr;

    auto copy = std::make_shared<mf::AssemblyNode>(*node);
    copy->children.clear();
    copy->parent.reset();

    for (const auto& child : node->children) {
        auto childCopy = cloneFilteredAssembly(child, excludedTokens, excluded);
        if (!childCopy) continue;
        childCopy->parent = copy;
        copy->children.push_back(childCopy);
    }

    return copy;
}

mf::AssemblyNodePtr cloneIncludedAssembly(
    const mf::AssemblyNodePtr& node,
    const std::unordered_set<std::string>& includedTokens,
    bool ancestorIncluded = false) {
    if (!node) return nullptr;
    const bool currentIncluded = ancestorIncluded || nodeMatchesAnyToken(node, includedTokens);

    std::vector<mf::AssemblyNodePtr> childCopies;
    for (const auto& child : node->children) {
        auto childCopy = cloneIncludedAssembly(child, includedTokens, currentIncluded);
        if (childCopy) childCopies.push_back(childCopy);
    }

    if (!currentIncluded && childCopies.empty()) return nullptr;

    auto copy = std::make_shared<mf::AssemblyNode>(*node);
    copy->children.clear();
    copy->parent.reset();
    for (auto& childCopy : childCopies) {
        childCopy->parent = copy;
        copy->children.push_back(childCopy);
    }
    return copy;
}

void collectAssemblyContents(
    const mf::AssemblyNodePtr& node,
    std::unordered_set<std::string>& partIds,
    std::unordered_set<std::string>& shapeKeys) {
    if (!node) return;
    if (node->type == mf::AssemblyNode::Type::Part) {
        if (!node->id.empty()) partIds.insert(node->id);
        if (!node->shapeKey.empty()) shapeKeys.insert(node->shapeKey);
    }
    for (const auto& child : node->children) collectAssemblyContents(child, partIds, shapeKeys);
}

std::unordered_set<std::string> makeTokenSet(const std::vector<std::string>& ids) {
    std::unordered_set<std::string> tokens;
    for (const auto& id : ids) {
        const std::string token = trimToken(id);
        if (!token.empty()) tokens.insert(token);
    }
    return tokens;
}

void retainAssemblyContents(const std::shared_ptr<mf::STEPResult>& step, const mf::AssemblyNodePtr& filteredRoot) {
    std::unordered_set<std::string> remainingPartIds;
    std::unordered_set<std::string> remainingShapeKeys;
    collectAssemblyContents(filteredRoot, remainingPartIds, remainingShapeKeys);
    if (remainingPartIds.empty() || remainingShapeKeys.empty()) {
        throw std::runtime_error("All parts were excluded; nothing remains to convert or export");
    }

    for (auto it = step->partsById.begin(); it != step->partsById.end();) {
        if (!remainingPartIds.contains(it->first)) it = step->partsById.erase(it);
        else ++it;
    }
    for (auto it = step->shapesByKey.begin(); it != step->shapesByKey.end();) {
        if (!remainingShapeKeys.contains(it->first)) it = step->shapesByKey.erase(it);
        else ++it;
    }
    for (auto it = step->instances.begin(); it != step->instances.end();) {
        std::erase_if(it->second, [&](const mf::AssemblyNodePtr& part) {
            return !part || part->id.empty() || !remainingPartIds.contains(part->id);
        });
        if (it->second.empty()) it = step->instances.erase(it);
        else ++it;
    }
    step->root = filteredRoot;
}

void applyNodeFilters(const std::shared_ptr<mf::STEPResult>& step, const Options& opts) {
    if (!step) return;

    const auto includeTokens = makeTokenSet(opts.includeNodeIds);
    if (!includeTokens.empty()) {
        auto includedRoot = cloneIncludedAssembly(step->root, includeTokens);
        if (!includedRoot) {
            throw std::runtime_error("All model nodes were excluded; nothing remains to convert or export");
        }
        retainAssemblyContents(step, includedRoot);
    }

    const auto excludedTokens = makeTokenSet(opts.excludeNodeIds);
    if (!excludedTokens.empty()) {
        auto filteredRoot = cloneFilteredAssembly(step->root, excludedTokens);
        if (!filteredRoot) {
            throw std::runtime_error("All model nodes were excluded; nothing remains to convert or export");
        }
        retainAssemblyContents(step, filteredRoot);
    }
}

json assemblyToJson(const mf::AssemblyNodePtr& node) {
    if (!node) return nullptr;
    json item;
    item["id"] = node->id;
    item["name"] = node->name;
    item["shapeKey"] = node->shapeKey;
    item["isInstance"] = node->isInstance;
    item["prototypeId"] = node->prototypeId;
    switch (node->type) {
        case mf::AssemblyNode::Type::Root: item["type"] = "root"; break;
        case mf::AssemblyNode::Type::Assembly: item["type"] = "assembly"; break;
        case mf::AssemblyNode::Type::Part: item["type"] = "part"; break;
    }
    item["children"] = json::array();
    for (const auto& child : node->children) {
        item["children"].push_back(assemblyToJson(child));
    }
    return item;
}

void appendAssemblyNode(
    const mf::AssemblyNodePtr& assemblyNode,
    const std::shared_ptr<mf::SceneNode>& parent,
    const std::unordered_map<std::string, std::shared_ptr<mf::LODMesh>>& meshesByShapeKey) {
    if (!assemblyNode || !parent) return;

    const bool isPart = assemblyNode->type == mf::AssemblyNode::Type::Part;
    const std::string sceneNodeName = !assemblyNode->id.empty()
        ? assemblyNode->id
        : (assemblyNode->name.empty() ? "node" : assemblyNode->name);
    auto sceneNode = std::make_shared<mf::SceneNode>(
        sceneNodeName,
        isPart ? mf::SceneNode::Type::Mesh : mf::SceneNode::Type::Group);
    sceneNode->setLocalMatrix(assemblyNode->localTransform);

    if (isPart) {
        auto meshIt = meshesByShapeKey.find(assemblyNode->shapeKey);
        if (meshIt != meshesByShapeKey.end()) {
            sceneNode->setMesh(meshIt->second);
        }
    }

    parent->addChild(sceneNode);
    for (const auto& child : assemblyNode->children) {
        appendAssemblyNode(child, sceneNode, meshesByShapeKey);
    }
}

std::shared_ptr<mf::SceneNode> makeSceneNode(
    const mf::AssemblyNodePtr& assemblyNode,
    const std::unordered_map<std::string, std::shared_ptr<mf::LODMesh>>& meshesByShapeKey) {
    if (!assemblyNode) return nullptr;

    const bool isPart = assemblyNode->type == mf::AssemblyNode::Type::Part;
    const std::string sceneNodeName = !assemblyNode->id.empty()
        ? assemblyNode->id
        : (assemblyNode->name.empty() ? "node" : assemblyNode->name);
    auto sceneNode = std::make_shared<mf::SceneNode>(
        sceneNodeName,
        isPart ? mf::SceneNode::Type::Mesh : mf::SceneNode::Type::Group);
    sceneNode->setLocalMatrix(assemblyNode->localTransform);

    if (isPart) {
        auto meshIt = meshesByShapeKey.find(assemblyNode->shapeKey);
        if (meshIt != meshesByShapeKey.end()) {
            sceneNode->setMesh(meshIt->second);
        }
    }

    for (const auto& child : assemblyNode->children) {
        auto childNode = makeSceneNode(child, meshesByShapeKey);
        if (childNode) sceneNode->addChild(childNode);
    }
    return sceneNode;
}

mf::MeshData mergeNodeMesh(mf::SceneNode* node) {
    mf::MeshData merged;
    if (!node) return merged;

    node->traverse([&merged](mf::SceneNode* descendant) {
        if (!descendant || descendant->type() != mf::SceneNode::Type::Mesh || !descendant->mesh()
            || descendant->mesh()->lods.empty()) {
            return;
        }

        const auto& md = descendant->mesh()->lods[0].mesh;
        if (md.vertices.empty() || md.indices.empty()) return;

        const uint32_t offset = static_cast<uint32_t>(merged.vertices.size());
        const mf::Mat4 world = descendant->worldTransform();
        for (const auto& vertex : md.vertices) {
            mf::Vertex transformed = vertex;
            transformed.position = mf::Vec3(world * mf::Vec4(vertex.position, 1.0f));
            transformed.normal = glm::normalize(mf::Vec3(world * mf::Vec4(vertex.normal, 0.0f)));
            merged.vertices.push_back(transformed);
        }
        for (const auto index : md.indices) {
            merged.indices.push_back(offset + index);
        }
    });

    if (!merged.vertices.empty()) {
        merged.computeAABB();
        merged.computeNormals();
    }
    return merged;
}

mf::MeshData mergeNodeMesh(const std::shared_ptr<mf::SceneNode>& node) {
    return mergeNodeMesh(node.get());
}

mf::MeshData makeMeshData(const mf::BRepMesh& brepMesh) {
    mf::MeshData mesh;
    mesh.vertices = brepMesh.vertices;
    mesh.indices = brepMesh.indices;
    mesh.aabb = brepMesh.aabb;
    mesh.computeAABB();
    if (!mesh.vertices.empty() && !mesh.indices.empty()) {
        mesh.computeNormals();
        mesh.computeTangents();
    }
    return mesh;
}

ConversionResult buildConversion(const Options& opts) {
    ConversionResult result;
    result.t0 = std::chrono::steady_clock::now();

    mf::STEPReader reader;
    result.step = reader.read(opts.input, opts.cacheDir);
    if (!result.step || !result.step->root) throw std::runtime_error("Could not parse CAD file");
    applyNodeFilters(result.step, opts);

    result.tParse = std::chrono::steady_clock::now();
    mf::BRepEngine brep;
    mf::TessellationParams tessellation;
    tessellation.linearDeflection = opts.linearDeflection;
    tessellation.angularDeflection = opts.angularDeflection;
    tessellation.relative = true;
    tessellation.parallelMeshing = true;

    std::unordered_map<std::string, std::shared_ptr<mf::LODMesh>> meshesByShapeKey;

    for (const auto& [shapeKey, shape] : result.step->shapesByKey) {
        mf::MeshData mesh = makeMeshData(brep.tessellate(shape, tessellation));
        if (mesh.triangleCount() == 0) {
            MF_WARN("Skipping empty shape: {}", shapeKey);
            continue;
        }

        ShapeStats shapeStats;
        shapeStats.key = shapeKey;
        shapeStats.verticesBefore = mesh.vertexCount();
        shapeStats.trianglesBefore = mesh.triangleCount();
        result.totalVerticesBefore += mesh.vertexCount();
        result.totalTrianglesBefore += mesh.triangleCount();

        mf::SimplifyParams simplifyParams;
        simplifyParams.targetRatio = opts.ratio;
        simplifyParams.targetError = opts.targetError;
        simplifyParams.maxPasses = 3;
        simplifyParams.useSloppyFallback = true;
        simplifyParams.preserveBoundary = false;
        if (opts.targetFileSizeMB.has_value()) {
            simplifyParams.mode = mf::SimplifyMode::TargetFileSizeMB;
            simplifyParams.targetFileSizeMB = *opts.targetFileSizeMB;
        }

        mf::SimplifyResult simplified;
        if (opts.ratio < 0.999f || opts.targetFileSizeMB.has_value()) {
            simplified = mf::Simplifier::simplify(mesh, simplifyParams);
        } else {
            simplified.mesh = mesh;
            simplified.achievedRatio = 1.0f;
        }
        simplified.mesh.computeAABB();
        simplified.mesh.computeNormals();
        simplified.mesh.computeTangents();

        shapeStats.verticesAfter = simplified.mesh.vertexCount();
        shapeStats.trianglesAfter = simplified.mesh.triangleCount();
        shapeStats.achievedRatio = simplified.achievedRatio;
        result.totalVerticesAfter += simplified.mesh.vertexCount();
        result.totalTrianglesAfter += simplified.mesh.triangleCount();

        auto lodMesh = std::make_shared<mf::LODMesh>();
        lodMesh->name = shapeKey;
        lodMesh->lods.push_back(mf::LODLevel{0, 0.0f, std::move(simplified.mesh)});
        meshesByShapeKey[shapeKey] = lodMesh;
        result.stats.push_back(shapeStats);
    }

    if (meshesByShapeKey.empty()) throw std::runtime_error("No tessellated meshes were produced");

    appendAssemblyNode(result.step->root, result.scene.root(), meshesByShapeKey);
    result.scene.detectInstances();
    return result;
}

json buildMetadata(const Options& opts, const ConversionResult& result, const std::chrono::steady_clock::time_point& tEnd) {
    json meta;
    meta["input"] = fs::absolute(opts.input).string();
    meta["output"] = opts.batchParts ? fs::absolute(opts.outputDir).string() : fs::absolute(opts.output).string();
    meta["format"] = opts.batchParts ? "stl-parts" : opts.format;
    meta["partCount"] = result.step->partsById.size();
    meta["shapeCount"] = result.step->shapesByKey.size();
    meta["verticesBefore"] = result.totalVerticesBefore;
    meta["trianglesBefore"] = result.totalTrianglesBefore;
    meta["verticesAfter"] = result.totalVerticesAfter;
    meta["trianglesAfter"] = result.totalTrianglesAfter;
    meta["reductionRatio"] = result.totalTrianglesBefore > 0
        ? 1.0 - static_cast<double>(result.totalTrianglesAfter) / static_cast<double>(result.totalTrianglesBefore)
        : 0.0;
    meta["settings"] = {
        {"ratio", opts.ratio},
        {"targetError", opts.targetError},
        {"linearDeflection", opts.linearDeflection},
        {"angularDeflection", opts.angularDeflection},
        {"targetFileSizeMB", opts.targetFileSizeMB ? json(*opts.targetFileSizeMB) : json(nullptr)},
        {"draco", opts.draco},
        {"excludedNodeIds", opts.excludeNodeIds},
        {"includeNodeIds", opts.includeNodeIds}
    };
    meta["timingsMs"] = {
        {"parse", std::chrono::duration_cast<std::chrono::milliseconds>(result.tParse - result.t0).count()},
        {"total", std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - result.t0).count()}
    };
    meta["parts"] = json::array();
    for (const auto& [partId, part] : result.step->partsById) {
        meta["parts"].push_back({
            {"id", partId},
            {"name", part ? part->name : ""},
            {"shapeKey", part ? part->shapeKey : ""},
            {"isInstance", part ? part->isInstance : false},
            {"prototypeId", part ? part->prototypeId : ""}
        });
    }
    meta["shapes"] = json::array();
    for (const auto& item : result.stats) {
        meta["shapes"].push_back({
            {"key", item.key},
            {"verticesBefore", item.verticesBefore},
            {"trianglesBefore", item.trianglesBefore},
            {"verticesAfter", item.verticesAfter},
            {"trianglesAfter", item.trianglesAfter},
            {"achievedRatio", item.achievedRatio}
        });
    }
    meta["hierarchy"] = assemblyToJson(result.step->root);
    return meta;
}

json exportBatchParts(const Options& opts, ConversionResult& result) {
    if (opts.outputDir.empty()) throw std::runtime_error("--output-dir is required with --batch-parts");
    fs::create_directories(opts.outputDir);

    json files = json::array();
    mf::STLExporter exporter;
    std::unordered_map<std::string, size_t> nameCounts;

    auto hasMeshContent = [](mf::SceneNode* node) {
        bool hasMesh = false;
        if (!node) return hasMesh;
        node->traverse([&hasMesh](mf::SceneNode* descendant) {
            if (hasMesh || !descendant) return;
            if (descendant->type() == mf::SceneNode::Type::Mesh && descendant->mesh()
                && !descendant->mesh()->lods.empty()) {
                hasMesh = true;
            }
        });
        return hasMesh;
    };

    auto isBatchExportWrapper = [](const std::shared_ptr<mf::SceneNode>& node) {
        if (!node || node->type() != mf::SceneNode::Type::Group) return false;
        const std::string& name = node->name();
        return name == "root" || name == "Root" || name == "Document" || name.rfind("Document_", 0) == 0;
    };

    std::vector<mf::SceneNode*> exportNodes;
    const auto* sourceNodes = &result.scene.root()->children();
    while (sourceNodes->size() == 1 && isBatchExportWrapper(sourceNodes->front())
           && !sourceNodes->front()->children().empty()) {
        sourceNodes = &sourceNodes->front()->children();
    }

    for (const auto& child : *sourceNodes) {
        if (!child || !hasMeshContent(child.get())) continue;
        exportNodes.push_back(child.get());
    }

    if (exportNodes.empty()) {
        result.scene.root()->traverse([&exportNodes](mf::SceneNode* node) {
            if (node && node->type() == mf::SceneNode::Type::Mesh && node->mesh() && !node->mesh()->lods.empty()) {
                exportNodes.push_back(node);
            }
        });
    }

    for (auto* exportNode : exportNodes) {
        mf::MeshData merged = mergeNodeMesh(exportNode);
        if (merged.vertices.empty() || merged.triangleCount() == 0) continue;

        std::string stem = sanitizeFileStem(exportNode->name());
        const size_t nextIndex = ++nameCounts[stem];
        if (nextIndex > 1) stem += "_" + std::to_string(nextIndex);

        const auto outPath = fs::path(opts.outputDir) / (stem + ".stl");
        if (!exporter.exportMesh(merged, outPath.string(), stem)) {
            throw std::runtime_error("STL part export failed: " + outPath.string());
        }
        const bool isAssembly = exportNode->type() == mf::SceneNode::Type::Group;
        files.push_back({
            {"name", exportNode->name()},
            {"fileName", outPath.filename().string()},
            {"path", fs::absolute(outPath).string()},
            {"type", isAssembly ? "assembly" : "part"},
            {"triangles", merged.triangleCount()}
        });
    }

    if (files.empty()) throw std::runtime_error("No part STL files were exported");
    return files;
}

int runSelfTest(const Options& opts) {
    mf::Scene scene;
    mf::MeshData mesh;
    mesh.vertices.push_back(mf::Vertex{mf::Vec3(-1, 0, 0), mf::Vec3(0, 0, 1), mf::Vec2(0, 0), mf::Vec4(1, 0, 0, 1)});
    mesh.vertices.push_back(mf::Vertex{mf::Vec3(1, 0, 0), mf::Vec3(0, 0, 1), mf::Vec2(1, 0), mf::Vec4(1, 0, 0, 1)});
    mesh.vertices.push_back(mf::Vertex{mf::Vec3(0, 1, 0), mf::Vec3(0, 0, 1), mf::Vec2(0.5f, 1), mf::Vec4(1, 0, 0, 1)});
    mesh.indices = {0, 1, 2};
    mesh.computeAABB();

    auto lodMesh = std::make_shared<mf::LODMesh>();
    lodMesh->name = "SelfTestTriangle";
    lodMesh->lods.push_back(mf::LODLevel{0, 0.0f, mesh});
    auto node = std::make_shared<mf::SceneNode>("SelfTestTriangle", mf::SceneNode::Type::Mesh);
    node->setMesh(lodMesh);
    scene.root()->addChild(node);

    std::string output = opts.output.empty() ? "self-test.glb" : opts.output;
    fs::create_directories(fs::absolute(output).parent_path());
    mf::glTFExportOptions exportOptions;
    exportOptions.useDraco = false;
    if (!mf::glTFExporter().exportScene(scene, output, exportOptions)) {
        std::cerr << "Self-test export failed\n";
        return 1;
    }
    std::cout << "Self-test exported: " << output << "\n";
    return 0;
}

int inspectCad(const Options& opts) {
    if (opts.input.empty()) throw std::runtime_error("--input is required");
    if (!fs::exists(opts.input)) throw std::runtime_error("Input file does not exist: " + opts.input);
    if (!opts.metadata.empty()) fs::create_directories(fs::absolute(opts.metadata).parent_path());
    if (!opts.cacheDir.empty()) fs::create_directories(opts.cacheDir);

    const auto t0 = std::chrono::steady_clock::now();
    mf::STEPReader reader;
    auto step = reader.read(opts.input, opts.cacheDir);
    if (!step || !step->root) throw std::runtime_error("Could not parse CAD file");
    const auto tEnd = std::chrono::steady_clock::now();

    json result;
    result["ok"] = true;
    result["input"] = fs::absolute(opts.input).string();
    result["partCount"] = step->partsById.size();
    result["shapeCount"] = step->shapesByKey.size();
    result["entityCount"] = step->entityCount;
    result["fileScale"] = step->fileScale;
    result["timingsMs"] = {
        {"inspect", std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count()}
    };
    result["hierarchy"] = assemblyToJson(step->root);
    result["parts"] = json::array();
    for (const auto& [partId, part] : step->partsById) {
        result["parts"].push_back({
            {"id", partId},
            {"name", part ? part->name : ""},
            {"shapeKey", part ? part->shapeKey : ""},
            {"isInstance", part ? part->isInstance : false},
            {"prototypeId", part ? part->prototypeId : ""}
        });
    }

    if (!opts.metadata.empty()) {
        std::ofstream out(opts.metadata);
        out << result.dump(2);
    }

    std::cout << result.dump() << "\n";
    return 0;
}

int convert(const Options& opts) {
    if (opts.input.empty()) throw std::runtime_error("--input is required");
    if (!opts.batchParts && opts.output.empty()) throw std::runtime_error("--output is required");
    if (!fs::exists(opts.input)) throw std::runtime_error("Input file does not exist: " + opts.input);

    if (!opts.output.empty()) fs::create_directories(fs::absolute(opts.output).parent_path());
    if (!opts.outputDir.empty()) fs::create_directories(opts.outputDir);
    if (!opts.metadata.empty()) fs::create_directories(fs::absolute(opts.metadata).parent_path());
    if (!opts.cacheDir.empty()) fs::create_directories(opts.cacheDir);

    ConversionResult result = buildConversion(opts);
    json batchFiles = json::array();
    if (opts.batchParts) {
        batchFiles = exportBatchParts(opts, result);
    } else if (opts.format == "stl") {
        if (!mf::STLExporter().exportScene(result.scene, opts.output)) {
            throw std::runtime_error("STL export failed: " + opts.output);
        }
    } else {
        mf::glTFExportOptions exportOptions;
        exportOptions.useDraco = opts.draco;
        exportOptions.embedBuffers = true;
        exportOptions.exportInstances = false;
        exportOptions.preserveHierarchy = true;

        if (!mf::glTFExporter().exportScene(result.scene, opts.output, exportOptions)) {
            throw std::runtime_error("GLB export failed: " + opts.output);
        }
    }

    const auto tEnd = std::chrono::steady_clock::now();
    json meta = buildMetadata(opts, result, tEnd);
    if (opts.batchParts) meta["files"] = batchFiles;
    if (!opts.metadata.empty()) {
        std::ofstream out(opts.metadata);
        out << meta.dump(2);
    }

    std::cout << json({
        {"ok", true},
        {"format", opts.batchParts ? "stl-parts" : opts.format},
        {"output", opts.batchParts ? fs::absolute(opts.outputDir).string() : fs::absolute(opts.output).string()},
        {"metadata", opts.metadata.empty() ? json(nullptr) : json(fs::absolute(opts.metadata).string())},
        {"files", opts.batchParts ? batchFiles : json::array()},
        {"trianglesBefore", result.totalTrianglesBefore},
        {"trianglesAfter", result.totalTrianglesAfter}
    }).dump() << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    mf::Logger::init();
    try {
        Options opts = parseArgs(argc, argv);
        if (opts.help) {
            printHelp();
            return 0;
        }
        if (opts.selfTest) {
            return runSelfTest(opts);
        }
        if (opts.inspect) {
            return inspectCad(opts);
        }
        return convert(opts);
    } catch (const std::exception& ex) {
        std::cerr << "MeshSimplifierCli error: " << ex.what() << "\n\n";
        printHelp();
        return 1;
    }
}
