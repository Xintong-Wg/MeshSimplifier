#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mf {

// ------------------------------------------------------------------
// Per-shape input for shell extraction
// ------------------------------------------------------------------
struct ShellShapeInput {
    std::string shapeKey;
    Mat4 worldTransform;    // local-to-world
    MeshData mesh;          // tessellated mesh in world space
};

// ------------------------------------------------------------------
// Shell extraction result
// ------------------------------------------------------------------
struct ShellExtractionResult {
    // Per-shape: per-triangle visibility (true = visible, keep)
    std::unordered_map<std::string, std::vector<bool>> triangleVisibility;

    // Shapes that are completely internal (all triangles invisible)
    std::unordered_set<std::string> fullyInternalShapes;

    // Per-shape visible mesh (internal triangles removed)
    std::unordered_map<std::string, MeshData> visibleMeshes;

    // Statistics
    size_t totalTrianglesBefore = 0;
    size_t totalTrianglesAfter = 0;
    float reductionRatio = 0.0f;
    float processingTimeMs = 0.0f;
};

// ------------------------------------------------------------------
// Voxel grid for visibility voting
// ------------------------------------------------------------------
struct VoxelGrid {
    glm::ivec3 resolution;
    float voxelSize;
    Vec3 origin;

    struct Voxel {
        bool occupied = false;
        uint32_t shapeIdx : 20;
        uint32_t faceIdx  : 20;
        uint32_t hitCount : 8;   // number of times this voxel was hit by rays
    };
    std::vector<Voxel> data;

    size_t index(int x, int y, int z) const {
        return static_cast<size_t>(z * resolution.y * resolution.x + y * resolution.x + x);
    }

    bool inBounds(int x, int y, int z) const {
        return x >= 0 && x < resolution.x &&
               y >= 0 && y < resolution.y &&
               z >= 0 && z < resolution.z;
    }
};

// ------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------
struct ShellExtractionParams {
    // Voxel resolution as fraction of bbox diagonal (default 1%)
    float voxelSizeFraction = 0.01f;

    // Fixed resolution override (0 = use fraction-based)
    uint32_t fixedResolution = 0;

    // Number of ray directions for voting (6, 14, or 26)
    uint32_t rayDirections = 6;

    // Ambient occlusion threshold for open boundaries
    float aoThreshold = 0.3f;
};

// ------------------------------------------------------------------
// Exterior shell extractor using Voxel Voting
// ------------------------------------------------------------------
class ShellExtractor {
public:
    ShellExtractor();
    ~ShellExtractor();

    // Extract visible exterior geometry
    ShellExtractionResult extract(const std::vector<ShellShapeInput>& shapes,
                                   const ShellExtractionParams& params = {});

    // Convenience: build visibility from a merged world-space mesh + part index ranges
    ShellExtractionResult extractFromMerged(
        const MeshData& mergedMesh,
        const std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>& partRanges,
        const ShellExtractionParams& params = {});

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mf
