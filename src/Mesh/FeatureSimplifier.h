#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include "Mesh/Simplifier.h"
#include "Geometry/SurfaceClassifier.h"
#include <vector>
#include <memory>

namespace mf {

// ------------------------------------------------------------------
// Feature-aware simplification parameters
// ------------------------------------------------------------------
struct FeatureAwareSimplifyParams {
    // Target
    float targetRatio = 0.25f;
    float maxError = 1e-2f;

    // Feature edge detection
    float featureEdgeDihedralAngle = 30.0f;  // degrees
    float featureEdgeWeight = 10.0f;          // QEM cost multiplier

    // Planar region protection
    float planarRegionWeight = 5.0f;
    bool lockPlanarBoundary = true;

    // Normal preservation
    float maxNormalDeviation = 45.0f;         // degrees
    bool enforceNormalCone = true;

    // Boundary
    bool preserveBoundary = false;
    float boundaryWeight = 100.0f;

    // Surface-aware weighting
    bool useSurfaceCategoryWeights = true;

    // When triangleTags are available, use per-triangle weights
    bool usePerTriangleTags = true;
};

// ------------------------------------------------------------------
// Feature edge detection result
// ------------------------------------------------------------------
struct FeatureEdges {
    // Per-edge: true if this edge is a feature edge
    std::vector<uint8_t> isFeatureEdge;  // indexed by edge

    // Per-vertex: number of feature edges incident
    std::vector<uint8_t> featureEdgeCount;

    // For checking feature edges: edge → vertex pair
    std::vector<std::pair<uint32_t, uint32_t>> edges;
};

// ------------------------------------------------------------------
// Planar region
// ------------------------------------------------------------------
struct PlanarRegion {
    std::vector<uint32_t> triangleIndices;
    Vec3 planeNormal;
    float planeOffset;
    float totalArea;
};

// ------------------------------------------------------------------
// Feature-preserving mesh simplifier
// ------------------------------------------------------------------
class FeatureSimplifier {
public:
    FeatureSimplifier();
    ~FeatureSimplifier();

    // Simplify with surface tags (preferred — uses per-triangle category)
    MeshData simplify(const MeshData& input,
                       const std::vector<SurfaceTag>& triangleTags,
                       const FeatureAwareSimplifyParams& params);

    // Simplify without surface tags (auto-detect features)
    MeshData simplify(const MeshData& input,
                       const FeatureAwareSimplifyParams& params);

    // Unified interface: simplify with visual-fidelity params + optional tags
    SimplifyResult simplifyV2(const MeshData& input,
                               const SimplifyParams& params,
                               const std::vector<SurfaceTag>* tags = nullptr);

    // Detect feature edges from mesh + optional tags
    static FeatureEdges detectFeatureEdges(const MeshData& mesh,
                                            const std::vector<SurfaceTag>* tags,
                                            float dihedralThresholdDeg);

    // Detect planar regions via region growing
    static std::vector<PlanarRegion> detectPlanarRegions(const MeshData& mesh,
                                                          const std::vector<SurfaceTag>* tags,
                                                          float normalThreshold = 0.01f);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mf
