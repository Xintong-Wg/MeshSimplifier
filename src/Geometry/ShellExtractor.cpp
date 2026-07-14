#include "Geometry/ShellExtractor.h"
#include "Core/Logger.h"

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>

namespace mf {

// ------------------------------------------------------------------
// 3D DDA (Digital Differential Analyzer) for triangle-to-voxel rasterization
// ------------------------------------------------------------------
// Rasterizes a triangle into a voxel grid marking occupied voxels.
static void rasterizeTriangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                               uint32_t shapeIdx, uint32_t faceIdx,
                               VoxelGrid& grid) {
    // Compute triangle AABB in voxel space
    auto worldToVoxel = [&](const Vec3& p) -> glm::ivec3 {
        Vec3 local = (p - grid.origin) / grid.voxelSize;
        return glm::ivec3(
            static_cast<int>(std::floor(local.x)),
            static_cast<int>(std::floor(local.y)),
            static_cast<int>(std::floor(local.z)));
    };

    glm::ivec3 v0v = worldToVoxel(v0);
    glm::ivec3 v1v = worldToVoxel(v1);
    glm::ivec3 v2v = worldToVoxel(v2);

    // Bounding box in voxel space
    glm::ivec3 bmin(
        std::max(0, std::min({v0v.x, v1v.x, v2v.x})),
        std::max(0, std::min({v0v.y, v1v.y, v2v.y})),
        std::max(0, std::min({v0v.z, v1v.z, v2v.z})));
    glm::ivec3 bmax(
        std::min(grid.resolution.x - 1, std::max({v0v.x, v1v.x, v2v.x})),
        std::min(grid.resolution.y - 1, std::max({v0v.y, v1v.y, v2v.y})),
        std::min(grid.resolution.z - 1, std::max({v0v.z, v1v.z, v2v.z})));

    // Barycentric test for each voxel in the bounding box
    Vec3 e1 = v1 - v0, e2 = v2 - v0;
    float d00 = glm::dot(e1, e1);
    float d01 = glm::dot(e1, e2);
    float d11 = glm::dot(e2, e2);
    float invDenom = 1.0f / (d00 * d11 - d01 * d01);

    for (int x = bmin.x; x <= bmax.x; ++x) {
        for (int y = bmin.y; y <= bmax.y; ++y) {
            for (int z = bmin.z; z <= bmax.z; ++z) {
                // Voxel center in world space
                Vec3 vc(
                    grid.origin.x + (x + 0.5f) * grid.voxelSize,
                    grid.origin.y + (y + 0.5f) * grid.voxelSize,
                    grid.origin.z + (z + 0.5f) * grid.voxelSize);

                Vec3 vp = vc - v0;
                float d20 = glm::dot(vp, e1);
                float d21 = glm::dot(vp, e2);
                float beta = (d11 * d20 - d01 * d21) * invDenom;
                float gamma = (d00 * d21 - d01 * d20) * invDenom;
                float alpha = 1.0f - beta - gamma;

                if (alpha >= -0.01f && beta >= -0.01f && gamma >= -0.01f) {
                    size_t idx = grid.index(x, y, z);
                    auto& vox = grid.data[idx];
                    vox.occupied = true;
                    vox.shapeIdx = shapeIdx;
                    vox.faceIdx = faceIdx;
                }
            }
        }
    }
}

// ------------------------------------------------------------------
// Generate ray directions for voting
// ------------------------------------------------------------------
static std::vector<Vec3> generateRayDirections(uint32_t count) {
    std::vector<Vec3> dirs;

    if (count <= 6) {
        // 6 axis-aligned directions
        dirs = {
            Vec3(1,0,0), Vec3(-1,0,0),
            Vec3(0,1,0), Vec3(0,-1,0),
            Vec3(0,0,1), Vec3(0,0,-1)
        };
    } else if (count <= 14) {
        // 6 axis + 8 corners
        dirs = {
            Vec3(1,0,0), Vec3(-1,0,0), Vec3(0,1,0), Vec3(0,-1,0),
            Vec3(0,0,1), Vec3(0,0,-1),
            Vec3(1,1,1), Vec3(-1,1,1), Vec3(1,-1,1), Vec3(1,1,-1),
            Vec3(-1,-1,1), Vec3(-1,1,-1), Vec3(1,-1,-1), Vec3(-1,-1,-1)
        };
        // Normalize corners
        for (size_t i = 6; i < dirs.size(); ++i) dirs[i] = glm::normalize(dirs[i]);
    } else {
        // 26 directions (6 faces + 12 edges + 8 corners)
        // Simplified: use 14 + some edge midpoints
        dirs = generateRayDirections(14);
        dirs.push_back(Vec3(1,1,0)); dirs.push_back(Vec3(1,-1,0));
        dirs.push_back(Vec3(-1,1,0)); dirs.push_back(Vec3(-1,-1,0));
        dirs.push_back(Vec3(1,0,1)); dirs.push_back(Vec3(1,0,-1));
        dirs.push_back(Vec3(-1,0,1)); dirs.push_back(Vec3(-1,0,-1));
        dirs.push_back(Vec3(0,1,1)); dirs.push_back(Vec3(0,1,-1));
        dirs.push_back(Vec3(0,-1,1)); dirs.push_back(Vec3(0,-1,-1));
        for (size_t i = 14; i < dirs.size(); ++i) dirs[i] = glm::normalize(dirs[i]);
    }

    return dirs;
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class ShellExtractor::Impl {
public:
    ShellExtractionResult extract(const std::vector<ShellShapeInput>& shapes,
                                   const ShellExtractionParams& params);
};

ShellExtractor::ShellExtractor() : m_impl(std::make_unique<Impl>()) {}
ShellExtractor::~ShellExtractor() = default;

ShellExtractionResult ShellExtractor::extract(
    const std::vector<ShellShapeInput>& shapes,
    const ShellExtractionParams& params) {
    return m_impl->extract(shapes, params);
}

ShellExtractionResult ShellExtractor::extractFromMerged(
    const MeshData& mergedMesh,
    const std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>& partRanges,
    const ShellExtractionParams& params) {

    // Decompose merged mesh into per-shape inputs
    std::vector<ShellShapeInput> shapes;
    for (auto& [key, range] : partRanges) {
        ShellShapeInput input;
        input.shapeKey = key;
        input.worldTransform = Mat4(1.0f); // already in world space

        uint32_t idxStart = range.first;
        uint32_t idxCount = range.second;

        // Extract vertex and index subset
        // Find min/max vertex indices used by this range
        uint32_t minVert = UINT32_MAX, maxVert = 0;
        for (uint32_t i = idxStart; i < idxStart + idxCount; ++i) {
            uint32_t vi = mergedMesh.indices[i];
            minVert = std::min(minVert, vi);
            maxVert = std::max(maxVert, vi);
        }

        if (maxVert >= mergedMesh.vertexCount()) continue;

        input.mesh.vertices.assign(
            mergedMesh.vertices.begin() + minVert,
            mergedMesh.vertices.begin() + maxVert + 1);
        input.mesh.indices.reserve(idxCount);
        for (uint32_t i = idxStart; i < idxStart + idxCount; ++i) {
            input.mesh.indices.push_back(mergedMesh.indices[i] - minVert);
        }

        shapes.push_back(std::move(input));
    }

    return m_impl->extract(shapes, params);
}

// ------------------------------------------------------------------
ShellExtractionResult ShellExtractor::Impl::extract(
    const std::vector<ShellShapeInput>& shapes,
    const ShellExtractionParams& params) {

    auto t0 = std::chrono::high_resolution_clock::now();
    ShellExtractionResult result;

    if (shapes.empty()) return result;

    // Step 1: Compute world AABB across all shapes
    AABB worldAABB;
    for (auto& s : shapes) {
        worldAABB.expand(s.mesh.aabb);
    }
    Vec3 worldDiag = worldAABB.extent();

    // Step 2: Build voxel grid
    VoxelGrid grid;
    uint32_t resolution;
    if (params.fixedResolution > 0) {
        resolution = params.fixedResolution;
    } else {
        resolution = static_cast<uint32_t>(1.0f / std::max(0.001f, params.voxelSizeFraction));
        resolution = std::max(32u, std::min(256u, resolution));
    }

    // Compute voxel size to cover the bbox (uniform voxels)
    float maxExtent = std::max({worldDiag.x, worldDiag.y, worldDiag.z});
    grid.voxelSize = maxExtent / static_cast<float>(resolution);
    grid.resolution = glm::ivec3(
        std::max(1, static_cast<int>(std::ceil(worldDiag.x / grid.voxelSize))),
        std::max(1, static_cast<int>(std::ceil(worldDiag.y / grid.voxelSize))),
        std::max(1, static_cast<int>(std::ceil(worldDiag.z / grid.voxelSize))));

    grid.origin = worldAABB.min - Vec3(grid.voxelSize); // small margin

    size_t totalVoxels = static_cast<size_t>(grid.resolution.x) *
                         static_cast<size_t>(grid.resolution.y) *
                         static_cast<size_t>(grid.resolution.z);
    grid.data.resize(totalVoxels);

    MF_INFO("ShellExtractor: voxel grid {}x{}x{} (~{}K voxels, size={:.4f})",
            grid.resolution.x, grid.resolution.y, grid.resolution.z,
            totalVoxels / 1024, grid.voxelSize);

    // Step 3: Rasterize all triangles into voxel grid
    result.totalTrianglesBefore = 0;
    for (size_t si = 0; si < shapes.size(); ++si) {
        const auto& shape = shapes[si];
        result.totalTrianglesBefore += shape.mesh.triangleCount();

        tbb::parallel_for(tbb::blocked_range<uint32_t>(0, shape.mesh.triangleCount()),
            [&](const tbb::blocked_range<uint32_t>& r) {
                for (uint32_t t = r.begin(); t < r.end(); ++t) {
                    uint32_t i0 = shape.mesh.indices[t*3];
                    uint32_t i1 = shape.mesh.indices[t*3+1];
                    uint32_t i2 = shape.mesh.indices[t*3+2];

                    rasterizeTriangle(
                        shape.mesh.vertices[i0].position,
                        shape.mesh.vertices[i1].position,
                        shape.mesh.vertices[i2].position,
                        static_cast<uint32_t>(si),
                        t, grid);
                }
            });
    }

    // Step 4: Multi-direction ray casting from boundary voxels
    auto rayDirs = generateRayDirections(params.rayDirections);

    // Find boundary voxels: occupied voxels with at least one empty neighbor
    // This is the set of voxels we fire rays from
    std::vector<glm::ivec3> boundaryVoxels;
    for (int x = 0; x < grid.resolution.x; ++x) {
        for (int y = 0; y < grid.resolution.y; ++y) {
            for (int z = 0; z < grid.resolution.z; ++z) {
                size_t idx = grid.index(x, y, z);
                if (!grid.data[idx].occupied) continue;

                // Check if any of the 6 neighbors is empty (boundary test)
                bool isBoundary = false;
                for (int dx = -1; dx <= 1 && !isBoundary; dx += 2) {
                    if (!grid.inBounds(x+dx, y, z) || !grid.data[grid.index(x+dx, y, z)].occupied)
                        isBoundary = true;
                }
                for (int dy = -1; dy <= 1 && !isBoundary; dy += 2) {
                    if (!grid.inBounds(x, y+dy, z) || !grid.data[grid.index(x, y+dy, z)].occupied)
                        isBoundary = true;
                }
                for (int dz = -1; dz <= 1 && !isBoundary; dz += 2) {
                    if (!grid.inBounds(x, y, z+dz) || !grid.data[grid.index(x, y, z+dz)].occupied)
                        isBoundary = true;
                }
                if (isBoundary) boundaryVoxels.push_back(glm::ivec3(x, y, z));
            }
        }
    }

    MF_INFO("ShellExtractor: {} boundary voxels, {} ray directions",
            boundaryVoxels.size(), rayDirs.size());

    // Step 5: Cast rays from outside the bbox toward boundary voxels
    // For each direction, we march from outside the grid inward
    tbb::parallel_for(tbb::blocked_range<size_t>(0, rayDirs.size()),
        [&](const tbb::blocked_range<size_t>& r) {
            for (size_t di = r.begin(); di < r.end(); ++di) {
                Vec3 dir = rayDirs[di];

                // Determine which faces of the bbox to start rays from
                // Ray marching from outside toward the interior
                for (auto& bv : boundaryVoxels) {
                    // Check if this is the first occupied voxel along -dir from outside
                    // Walk backward along -dir
                    glm::ivec3 probe = bv;
                    bool isFirstHit = true;
                    for (int s = 0; s < 3; ++s) { // check a few steps back
                        probe.x -= static_cast<int>(std::round(dir.x));
                        probe.y -= static_cast<int>(std::round(dir.y));
                        probe.z -= static_cast<int>(std::round(dir.z));
                        if (grid.inBounds(probe.x, probe.y, probe.z)) {
                            if (grid.data[grid.index(probe.x, probe.y, probe.z)].occupied) {
                                isFirstHit = false;
                                break;
                            }
                        }
                    }

                    if (isFirstHit) {
                        // This boundary voxel is the first hit from outside
                        size_t idx = grid.index(bv.x, bv.y, bv.z);
                        grid.data[idx].hitCount = std::min(255u,
                            static_cast<uint32_t>(grid.data[idx].hitCount) + 1);
                    }
                }
            }
        });

    // Step 6: Determine visibility based on hit counts
    // A triangle is visible if at least one voxel it occupies has hitCount > 0
    for (size_t si = 0; si < shapes.size(); ++si) {
        const auto& shape = shapes[si];
        std::vector<bool> vis(shape.mesh.triangleCount(), false);
        MeshData visibleMesh;
        uint32_t visibleTris = 0;

        // Check each triangle's visibility
        for (uint32_t t = 0; t < shape.mesh.triangleCount(); ++t) {
            uint32_t i0 = shape.mesh.indices[t*3];
            uint32_t i1 = shape.mesh.indices[t*3+1];
            uint32_t i2 = shape.mesh.indices[t*3+2];

            // Get triangle center in voxel space
            Vec3 center = (shape.mesh.vertices[i0].position +
                           shape.mesh.vertices[i1].position +
                           shape.mesh.vertices[i2].position) / 3.0f;

            Vec3 local = (center - grid.origin) / grid.voxelSize;
            int cx = static_cast<int>(std::floor(local.x));
            int cy = static_cast<int>(std::floor(local.y));
            int cz = static_cast<int>(std::floor(local.z));

            bool visible = false;
            if (grid.inBounds(cx, cy, cz)) {
                size_t idx = grid.index(cx, cy, cz);
                if (grid.data[idx].hitCount > 0) {
                    visible = true;
                }
            } else {
                // Outside grid but occupied = visible (shouldn't happen normally)
                visible = true;
            }

            vis[t] = visible;
            if (visible) ++visibleTris;
        }

        result.triangleVisibility[shape.shapeKey] = std::move(vis);
        result.totalTrianglesAfter += visibleTris;

        if (visibleTris == 0) {
            result.fullyInternalShapes.insert(shape.shapeKey);
        }
    }

    result.reductionRatio = result.totalTrianglesBefore > 0
        ? 1.0f - static_cast<float>(result.totalTrianglesAfter) /
                 static_cast<float>(result.totalTrianglesBefore)
        : 0.0f;

    auto t1 = std::chrono::high_resolution_clock::now();
    result.processingTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    MF_INFO("ShellExtractor: {} → {} triangles ({:.1f}% reduction, {:.1f}ms, {} shapes fully internal)",
            result.totalTrianglesBefore, result.totalTrianglesAfter,
            result.reductionRatio * 100.0f, result.processingTimeMs,
            result.fullyInternalShapes.size());

    return result;
}

} // namespace mf
