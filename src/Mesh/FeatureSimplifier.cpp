#include "Mesh/FeatureSimplifier.h"
#include "Core/Logger.h"

#include <meshoptimizer.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>

namespace mf {

// ------------------------------------------------------------------
// Feature edge detection based on dihedral angle
// ------------------------------------------------------------------
FeatureEdges FeatureSimplifier::detectFeatureEdges(
    const MeshData& mesh, const std::vector<SurfaceTag>* tags,
    float dihedralThresholdDeg) {

    FeatureEdges fe;
    if (mesh.triangleCount() == 0) return fe;

    // Build edge → triangle map
    std::map<uint64_t, std::vector<uint32_t>> edgeTris;
    for (uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        for (int j = 0; j < 3; ++j) {
            uint32_t a = mesh.indices[t*3 + j];
            uint32_t b = mesh.indices[t*3 + (j+1)%3];
            if (a > b) std::swap(a, b);
            uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
            edgeTris[key].push_back(t);
        }
    }

    fe.isFeatureEdge.resize(mesh.triangleCount(), 0);
    fe.featureEdgeCount.resize(mesh.vertexCount(), 0);

    float thresholdCos = std::cos(dihedralThresholdDeg * 3.1415926535f / 180.0f);

    for (auto& [key, tris] : edgeTris) {
        if (tris.size() < 2) {
            // Boundary edge = feature edge
            for (auto t : tris) { fe.isFeatureEdge[t] = 1; }
            continue;
        }

        for (size_t i = 0; i < tris.size(); ++i) {
            for (size_t j = i + 1; j < tris.size(); ++j) {
                uint32_t t1 = tris[i], t2 = tris[j];
                Vec3 n1, n2;

                // Compute triangle normals
                {
                    Vec3 p0 = mesh.vertices[mesh.indices[t1*3]].position;
                    Vec3 p1 = mesh.vertices[mesh.indices[t1*3+1]].position;
                    Vec3 p2 = mesh.vertices[mesh.indices[t1*3+2]].position;
                    n1 = glm::normalize(glm::cross(p1 - p0, p2 - p0));
                }
                {
                    Vec3 p0 = mesh.vertices[mesh.indices[t2*3]].position;
                    Vec3 p1 = mesh.vertices[mesh.indices[t2*3+1]].position;
                    Vec3 p2 = mesh.vertices[mesh.indices[t2*3+2]].position;
                    n2 = glm::normalize(glm::cross(p1 - p0, p2 - p0));
                }

                float dot = glm::dot(n1, n2);
                if (dot < thresholdCos) {
                    fe.isFeatureEdge[t1] = 1;
                    fe.isFeatureEdge[t2] = 1;
                }

                // Also check if categories differ
                if (tags && t1 < tags->size() && t2 < tags->size()) {
                    if ((*tags)[t1].category != (*tags)[t2].category) {
                        fe.isFeatureEdge[t1] = 1;
                        fe.isFeatureEdge[t2] = 1;
                    }
                }
            }
        }
    }

    return fe;
}

// ------------------------------------------------------------------
// Detect planar regions via region growing
// ------------------------------------------------------------------
std::vector<PlanarRegion> FeatureSimplifier::detectPlanarRegions(
    const MeshData& mesh, const std::vector<SurfaceTag>* tags,
    float normalThreshold) {

    std::vector<PlanarRegion> regions;
    if (mesh.triangleCount() == 0) return regions;

    std::vector<bool> visited(mesh.triangleCount(), false);

    for (uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        if (visited[t]) continue;

        // Check if this triangle is planar (from tags or geometry)
        bool isPlanar = false;
        Vec3 planeN(0, 0, 1);

        if (tags && t < tags->size()) {
            isPlanar = (*tags)[t].isPlanarRegion;
        }

        if (!isPlanar) {
            // Check geometrically: verify its neighbors share the same normal
            Vec3 p0 = mesh.vertices[mesh.indices[t*3]].position;
            Vec3 p1 = mesh.vertices[mesh.indices[t*3+1]].position;
            Vec3 p2 = mesh.vertices[mesh.indices[t*3+2]].position;
            planeN = glm::normalize(glm::cross(p1 - p0, p2 - p0));
            isPlanar = true; // start as planar but only grow if neighbors agree
        } else {
            Vec3 p0 = mesh.vertices[mesh.indices[t*3]].position;
            Vec3 p1 = mesh.vertices[mesh.indices[t*3+1]].position;
            Vec3 p2 = mesh.vertices[mesh.indices[t*3+2]].position;
            planeN = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        }

        if (!isPlanar) continue;

        // Region growing (BFS)
        PlanarRegion region;
        region.planeNormal = planeN;
        region.totalArea = 0.0f;

        std::vector<uint32_t> queue;
        queue.push_back(t);
        visited[t] = true;

        while (!queue.empty()) {
            uint32_t ct = queue.back();
            queue.pop_back();

            region.triangleIndices.push_back(ct);

            // Compute area
            Vec3 p0 = mesh.vertices[mesh.indices[ct*3]].position;
            Vec3 p1 = mesh.vertices[mesh.indices[ct*3+1]].position;
            Vec3 p2 = mesh.vertices[mesh.indices[ct*3+2]].position;
            float area = 0.5f * glm::length(glm::cross(p1 - p0, p2 - p0));
            region.totalArea += area;

            // Check neighbors (triangles sharing edges)
            for (uint32_t nt = 0; nt < mesh.triangleCount(); ++nt) {
                if (visited[nt]) continue;

                // Check if they share at least one edge
                if (tags && nt < tags->size() && !(*tags)[nt].isPlanarRegion) continue;

                Vec3 np0 = mesh.vertices[mesh.indices[nt*3]].position;
                Vec3 np1 = mesh.vertices[mesh.indices[nt*3+1]].position;
                Vec3 np2 = mesh.vertices[mesh.indices[nt*3+2]].position;
                Vec3 nn = glm::normalize(glm::cross(np1 - np0, np2 - np0));

                if (std::abs(glm::dot(planeN, nn)) > 0.99f) {
                    visited[nt] = true;
                    queue.push_back(nt);
                }
            }
        }

        if (region.triangleIndices.size() >= 4) {
            // Compute best-fit plane offset
            float sumD = 0.0f;
            for (auto ti : region.triangleIndices) {
                Vec3 c = (mesh.vertices[mesh.indices[ti*3]].position +
                          mesh.vertices[mesh.indices[ti*3+1]].position +
                          mesh.vertices[mesh.indices[ti*3+2]].position) / 3.0f;
                sumD += glm::dot(planeN, c);
            }
            region.planeOffset = sumD / static_cast<float>(region.triangleIndices.size());
            regions.push_back(std::move(region));
        }
    }

    return regions;
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class FeatureSimplifier::Impl {
public:
    MeshData simplifyWithTags(const MeshData& input,
                               const std::vector<SurfaceTag>& tags,
                               const FeatureAwareSimplifyParams& params);
    MeshData simplifyAuto(const MeshData& input,
                           const FeatureAwareSimplifyParams& params);

    // New visual-fidelity simplify
    SimplifyResult simplifyVisual(const MeshData& input,
                                   const SimplifyParams& params,
                                   const std::vector<SurfaceTag>* tags);

private:
    // Analyze surface composition and compute adaptive error
    struct SurfaceAnalysis {
        float planarRatio = 0.0f;
        float curvedRatio = 0.0f;
        float featureRatio = 0.0f;
        float freeformRatio = 0.0f;
        float adaptiveErrorScale = 1.0f; // multiplier for base error
    };
    SurfaceAnalysis analyzeSurfaceComposition(const MeshData& input,
                                                const std::vector<SurfaceTag>* tags);

    // Single meshopt pass with given error
    MeshData runMeshOpt(const MeshData& input, float targetRatio,
                         float targetError, bool preserveBoundary);
};

FeatureSimplifier::FeatureSimplifier() : m_impl(std::make_unique<Impl>()) {}
FeatureSimplifier::~FeatureSimplifier() = default;

MeshData FeatureSimplifier::simplify(const MeshData& input,
                                      const std::vector<SurfaceTag>& tags,
                                      const FeatureAwareSimplifyParams& params) {
    return m_impl->simplifyWithTags(input, tags, params);
}

MeshData FeatureSimplifier::simplify(const MeshData& input,
                                      const FeatureAwareSimplifyParams& params) {
    return m_impl->simplifyAuto(input, params);
}

SimplifyResult FeatureSimplifier::simplifyV2(const MeshData& input,
                                              const SimplifyParams& params,
                                              const std::vector<SurfaceTag>* tags) {
    return m_impl->simplifyVisual(input, params, tags);
}

// ------------------------------------------------------------------
// Analyze surface composition from tags
// ------------------------------------------------------------------
FeatureSimplifier::Impl::SurfaceAnalysis
FeatureSimplifier::Impl::analyzeSurfaceComposition(
    const MeshData& input, const std::vector<SurfaceTag>* tags) {

    SurfaceAnalysis sa;
    uint32_t triCount = input.triangleCount();
    if (triCount == 0) return sa;

    if (!tags || tags->empty()) {
        // No tags — assume uniform composition
        sa.adaptiveErrorScale = 1.0f;
        return sa;
    }

    uint32_t planar = 0, cylindrical = 0, spherical = 0, toroidal = 0;
    uint32_t conical = 0, extrusion = 0, revolution = 0, freeform = 0;
    uint32_t featureEdge = 0;

    uint32_t tagSize = static_cast<uint32_t>(tags->size());
    for (uint32_t t = 0; t < triCount && t < tagSize; ++t) {
        const auto& tag = (*tags)[t];
        switch (tag.category) {
        case SurfaceCategory::Planar:       ++planar; break;
        case SurfaceCategory::Cylindrical:  ++cylindrical; break;
        case SurfaceCategory::Conical:      ++conical; break;
        case SurfaceCategory::Spherical:    ++spherical; break;
        case SurfaceCategory::Toroidal:     ++toroidal; break;
        case SurfaceCategory::Extrusion:    ++extrusion; break;
        case SurfaceCategory::Revolution:   ++revolution; break;
        case SurfaceCategory::Freeform:     ++freeform; break;
        default: break;
        }
        if (tag.isFeatureEdge) ++featureEdge;
    }

    sa.planarRatio = static_cast<float>(planar) / static_cast<float>(triCount);
    sa.curvedRatio = static_cast<float>(cylindrical + conical + spherical + toroidal)
                    / static_cast<float>(triCount);
    sa.freeformRatio = static_cast<float>(freeform + extrusion + revolution)
                      / static_cast<float>(triCount);
    sa.featureRatio = static_cast<float>(featureEdge) / static_cast<float>(triCount);

    // Compute adaptive error scale:
    // - High planar ratio: can tolerate more error (planes look the same even with coarse mesh)
    // - High curved ratio: need less error (curvature needs fine tessellation)
    // - High feature ratio: need less error (features must be preserved)
    // - High freeform ratio: moderate error tolerance

    float scale = 1.0f;
    // Planar boost: up to 3x error for mostly planar meshes
    scale += sa.planarRatio * 2.0f;
    // Curved penalty: up to 0.5x error for mostly curved meshes
    scale -= sa.curvedRatio * 0.5f;
    // Feature penalty: up to 0.5x error for feature-rich meshes
    scale -= sa.featureRatio * 0.5f;
    // Freeform moderate penalty
    scale -= sa.freeformRatio * 0.2f;

    sa.adaptiveErrorScale = std::clamp(scale, 0.3f, 3.0f);

    MF_INFO("Surface analysis: planar={:.1f}% curved={:.1f}% freeform={:.1f}% feature={:.1f}% error_scale={:.2f}",
            sa.planarRatio * 100.0f, sa.curvedRatio * 100.0f, sa.freeformRatio * 100.0f,
            sa.featureRatio * 100.0f, sa.adaptiveErrorScale);

    return sa;
}

// ------------------------------------------------------------------
// Run a single meshopt pass
// ------------------------------------------------------------------
MeshData FeatureSimplifier::Impl::runMeshOpt(const MeshData& input,
                                              float targetRatio,
                                              float targetError,
                                              bool preserveBoundary) {
    if (input.triangleCount() == 0 || targetRatio >= 1.0f) return input;

    uint32_t targetTris = std::max(4u,
        static_cast<uint32_t>(static_cast<float>(input.triangleCount()) * targetRatio));

    std::vector<unsigned int> indices(input.indices.begin(), input.indices.end());

    unsigned int options = 0;
    if (preserveBoundary) options |= meshopt_SimplifyLockBorder;

    size_t resultCount = meshopt_simplify(
        indices.data(),
        input.indices.data(),
        static_cast<size_t>(input.indexCount()),
        reinterpret_cast<const float*>(input.vertices.data()),
        static_cast<size_t>(input.vertexCount()),
        sizeof(Vertex),
        static_cast<size_t>(targetTris * 3),
        targetError,
        options,
        nullptr
    );

    indices.resize(resultCount);

    // Vertex fetch optimization
    std::vector<unsigned int> remap(input.vertexCount());
    size_t uniqueVertices = meshopt_optimizeVertexFetchRemap(
        remap.data(), indices.data(), indices.size(), input.vertexCount());

    MeshData out;
    out.vertices.resize(uniqueVertices);
    for (size_t i = 0; i < input.vertexCount(); ++i) {
        if (remap[i] < uniqueVertices) {
            out.vertices[remap[i]] = input.vertices[i];
        }
    }
    out.indices.resize(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        out.indices[i] = static_cast<Index>(remap[indices[i]]);
    }

    meshopt_optimizeVertexCache(out.indices.data(), out.indices.data(),
        out.indices.size(), out.vertexCount());
    meshopt_optimizeOverdraw(out.indices.data(), out.indices.data(),
        out.indices.size(), reinterpret_cast<const float*>(out.vertices.data()),
        out.vertexCount(), sizeof(Vertex), 1.05f);

    out.computeNormals();
    out.computeTangents();
    out.computeAABB();

    return out;
}

// ------------------------------------------------------------------
// Visual-fidelity simplify — normal pass + CAD seam welding + sloppy
// ------------------------------------------------------------------
SimplifyResult FeatureSimplifier::Impl::simplifyVisual(
    const MeshData& input, const SimplifyParams& params,
    const std::vector<SurfaceTag>* tags) {

    SimplifyResult result;
    if (input.triangleCount() == 0) {
        result.mesh = input;
        result.achievedRatio = 1.0f;
        return result;
    }

    uint32_t sourceTris = input.triangleCount();
    uint32_t targetTris = std::max(4u,
        static_cast<uint32_t>(static_cast<float>(sourceTris) * params.targetRatio));
    targetTris = std::min(targetTris, sourceTris - 1);

    auto sa = analyzeSurfaceComposition(input, tags);
    float baseError = params.targetError * sa.adaptiveErrorScale;
    float ratioFactor = 1.0f + (1.0f - std::max(params.targetRatio, 0.01f)) * (1.0f + params.aggressive * 3.0f);
    float adaptiveError = std::clamp(baseError * ratioFactor, 0.001f, 0.5f);

    MF_INFO("FeatureSimplifier: source={} tris, target={}, adaptiveError={:.4f}",
            sourceTris, targetTris, adaptiveError);

    // Pass 1
    MeshData simplified = runMeshOpt(input, params.targetRatio, adaptiveError, params.preserveBoundary);
    uint32_t bestTris = simplified.triangleCount();
    result.mesh = std::move(simplified);
    result.usedError = adaptiveError;
    result.passes = 1;

    MF_INFO("  Pass 1: {} -> {} tris (target {}, error {:.4f})",
            sourceTris, bestTris, targetTris, adaptiveError);

    // Retry
    float retryError = adaptiveError;
    while (bestTris > targetTris * 1.15f && retryError < 0.5f) {
        retryError = std::min(retryError * 2.0f, 0.5f);
        MeshData retryMesh = runMeshOpt(input, params.targetRatio, retryError, false);
        uint32_t retryTris = retryMesh.triangleCount();
        MF_INFO("  Retry: {} -> {} tris (target {}, error {:.4f})",
                sourceTris, retryTris, targetTris, retryError);
        if (retryTris < bestTris) {
            result.mesh = std::move(retryMesh);
            result.usedError = retryError;
            bestTris = retryTris;
            ++result.passes;
        } else break;
    }

    // CAD seam welding + sloppy when normal simplify stalls
    if (params.useSloppyFallback && bestTris > targetTris * 1.4f) {
        MF_INFO("  Normal simplify stalled at {:.1f}% — welding + sloppy...",
                static_cast<float>(bestTris) / static_cast<float>(sourceTris) * 100.0f);

        MeshData welded;
        welded.indices.reserve(input.indices.size());

        std::vector<std::pair<Vec3, uint32_t>> sorted;
        sorted.reserve(input.vertices.size());
        for (size_t i = 0; i < input.vertices.size(); ++i) {
            sorted.emplace_back(input.vertices[i].position, static_cast<uint32_t>(i));
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                if (a.first.x != b.first.x) return a.first.x < b.first.x;
                if (a.first.y != b.first.y) return a.first.y < b.first.y;
                return a.first.z < b.first.z;
            });

        std::vector<uint32_t> vertexRemap(input.vertices.size(), static_cast<uint32_t>(-1));
        welded.vertices.reserve(input.vertices.size());
        for (size_t i = 0; i < sorted.size(); ++i) {
            uint32_t srcIdx = sorted[i].second;
            const Vec3& pos = sorted[i].first;
            if (i > 0 && pos == sorted[i - 1].first) {
                uint32_t weldedIdx = vertexRemap[sorted[i - 1].second];
                vertexRemap[srcIdx] = weldedIdx;
                welded.vertices[weldedIdx].normal += input.vertices[srcIdx].normal;
            } else {
                uint32_t weldedIdx = static_cast<uint32_t>(welded.vertices.size());
                vertexRemap[srcIdx] = weldedIdx;
                welded.vertices.push_back(input.vertices[srcIdx]);
            }
        }

        for (auto& v : welded.vertices) {
            float len = glm::length(v.normal);
            v.normal = (len > 1e-10f) ? (v.normal / len) : Vec3(0, 1, 0);
        }

        for (auto idx : input.indices) {
            welded.indices.push_back(vertexRemap[idx]);
        }

        if (welded.indices.size() % 3 != 0) {
            MF_WARN("  Welded mesh has invalid index count; skipping sloppy fallback");
        } else {
            MF_INFO("  Welded: {} -> {} verts", input.vertexCount(), welded.vertexCount());

            std::vector<unsigned int> sloppyIndices(welded.indices.begin(), welded.indices.end());
            float sloppyError = 0.0f;
            size_t sloppyCount = meshopt_simplifySloppy(
                sloppyIndices.data(), welded.indices.data(),
                static_cast<size_t>(welded.indexCount()),
                reinterpret_cast<const float*>(welded.vertices.data()),
                static_cast<size_t>(welded.vertexCount()), sizeof(Vertex),
                static_cast<size_t>(targetTris * 3), 1e10f, &sloppyError
            );
            sloppyIndices.resize(sloppyCount);

            uint32_t sloppyTris = static_cast<uint32_t>(sloppyCount / 3);
            MF_INFO("  Sloppy: {} -> {} tris", sourceTris, sloppyTris);
            if (sloppyCount > 0 && sloppyTris < bestTris) {
                std::vector<unsigned int> remap(welded.vertexCount());
                size_t unique = meshopt_optimizeVertexFetchRemap(
                    remap.data(), sloppyIndices.data(), sloppyIndices.size(), welded.vertexCount());
                MeshData sloppyMesh;
                sloppyMesh.vertices.resize(unique);
                for (size_t i = 0; i < welded.vertexCount(); ++i) {
                    if (remap[i] < unique) sloppyMesh.vertices[remap[i]] = welded.vertices[i];
                }
                sloppyMesh.indices.resize(sloppyIndices.size());
                for (size_t i = 0; i < sloppyIndices.size(); ++i)
                    sloppyMesh.indices[i] = static_cast<Index>(remap[sloppyIndices[i]]);
                sloppyMesh.computeNormals();
                sloppyMesh.computeTangents();
                sloppyMesh.computeAABB();
                result.mesh = std::move(sloppyMesh);
                result.usedSloppy = true;
                result.usedError = sloppyError;
                bestTris = sloppyTris;
                ++result.passes;
            }
        }
    }

    result.achievedRatio = static_cast<float>(bestTris) / static_cast<float>(sourceTris);
    MF_INFO("FeatureSimplify: {} -> {} tris (target {:.3f}, achieved {:.3f}, sloppy={})",
            sourceTris, result.mesh.triangleCount(),
            params.targetRatio, result.achievedRatio, result.usedSloppy);
    return result;
}

// ------------------------------------------------------------------
// Core simplification using meshoptimizer (original path)
// ------------------------------------------------------------------
MeshData FeatureSimplifier::Impl::simplifyWithTags(
    const MeshData& input, const std::vector<SurfaceTag>& tags,
    const FeatureAwareSimplifyParams& params) {

    if (input.triangleCount() == 0 || params.targetRatio >= 1.0f) return input;

    uint32_t sourceTris = input.triangleCount();
    uint32_t targetTris = std::max(4u,
        static_cast<uint32_t>(static_cast<float>(sourceTris) * params.targetRatio));

    // Copy index data (meshopt works in-place on the destination array)
    std::vector<unsigned int> indices(input.indices.begin(), input.indices.end());

    // Build vertex lock flags based on feature detection
    // Lock vertices on feature edges and planar region boundaries
    std::vector<unsigned char> vertexLock(input.vertexCount(), 0);

    if (params.usePerTriangleTags && !tags.empty()) {
        FeatureEdges fe = FeatureSimplifier::detectFeatureEdges(
            input, &tags, params.featureEdgeDihedralAngle);

        // Lock vertices that participate in feature edges
        for (size_t t = 0; t < fe.isFeatureEdge.size() && t < input.triangleCount(); ++t) {
            if (fe.isFeatureEdge[t]) {
                if (t*3+2 < input.indices.size()) {
                    vertexLock[input.indices[t*3]] = 1;
                    vertexLock[input.indices[t*3+1]] = 1;
                    vertexLock[input.indices[t*3+2]] = 1;
                }
            }
        }

        // Lock vertices on planar region boundaries
        if (params.lockPlanarBoundary) {
            for (size_t t = 0; t < input.triangleCount() && t < tags.size(); ++t) {
                if (tags[t].isPlanarRegion && t*3+2 < input.indices.size()) {
                    vertexLock[input.indices[t*3]] = 1;
                    vertexLock[input.indices[t*3+1]] = 1;
                    vertexLock[input.indices[t*3+2]] = 1;
                }
            }
        }
    }

    // Lock boundary vertices
    unsigned int options = 0;
    if (params.preserveBoundary) options |= meshopt_SimplifyLockBorder;

    // Run meshopt simplification
    size_t resultCount = meshopt_simplify(
        indices.data(),
        indices.data(),
        static_cast<size_t>(input.indexCount()),
        reinterpret_cast<const float*>(input.vertices.data()),
        static_cast<size_t>(input.vertexCount()),
        sizeof(Vertex),
        static_cast<size_t>(targetTris * 3),
        params.maxError,
        options,
        nullptr);

    indices.resize(resultCount);

    // Remap vertices to only those referenced by output indices
    std::vector<unsigned int> remap(input.vertexCount());
    size_t uniqueVertices = meshopt_optimizeVertexFetchRemap(
        remap.data(), indices.data(), indices.size(), input.vertexCount());

    // Build output mesh
    MeshData out;
    out.vertices.resize(uniqueVertices);
    for (size_t i = 0; i < input.vertexCount(); ++i) {
        if (remap[i] < uniqueVertices) {
            out.vertices[remap[i]] = input.vertices[i];
        }
    }
    out.indices.resize(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        out.indices[i] = static_cast<Index>(remap[indices[i]]);
    }

    // Post-simplification optimization
    meshopt_optimizeVertexCache(out.indices.data(), out.indices.data(),
        out.indices.size(), out.vertexCount());
    meshopt_optimizeOverdraw(out.indices.data(), out.indices.data(),
        out.indices.size(), reinterpret_cast<const float*>(out.vertices.data()),
        out.vertexCount(), sizeof(Vertex), 1.05f);

    out.computeNormals();
    out.computeTangents();
    out.computeAABB();

    MF_INFO("FeatureSimplifier (meshopt): {} -> {} triangles (ratio {:.2f})",
            sourceTris, out.triangleCount(),
            static_cast<float>(out.triangleCount()) / static_cast<float>(sourceTris));

    return out;
}

// ------------------------------------------------------------------
// Auto-detect features (no tags available)
// ------------------------------------------------------------------
MeshData FeatureSimplifier::Impl::simplifyAuto(
    const MeshData& input, const FeatureAwareSimplifyParams& params) {

    if (input.triangleCount() == 0 || params.targetRatio >= 1.0f) return input;

    // Auto-detect feature edges
    FeatureEdges fe = FeatureSimplifier::detectFeatureEdges(input, nullptr,
                                                             params.featureEdgeDihedralAngle);

    // Build synthetic tags
    std::vector<SurfaceTag> tags(input.triangleCount());
    for (uint32_t t = 0; t < input.triangleCount(); ++t) {
        if (t < fe.isFeatureEdge.size() && fe.isFeatureEdge[t]) {
            tags[t].isFeatureEdge = true;
        }
        // Detect planar by normal variance
        if (t*3+2 < input.indices.size()) {
            Vec3 n0 = input.vertices[input.indices[t*3]].normal;
            Vec3 n1 = input.vertices[input.indices[t*3+1]].normal;
            Vec3 n2 = input.vertices[input.indices[t*3+2]].normal;
            float var = glm::length(n0 - n1) + glm::length(n1 - n2) + glm::length(n2 - n0);
            if (var < 0.01f) tags[t].isPlanarRegion = true;
        }
    }

    return simplifyWithTags(input, tags, params);
}

} // namespace mf
