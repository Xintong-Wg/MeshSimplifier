#pragma once

#include "Geometry/BRepAnalyzer.h"
#include "Core/Types.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>

namespace mf {

// ------------------------------------------------------------------
// Tessellation strategy assigned to each face
// ------------------------------------------------------------------
enum class TessellationStrategy : uint8_t {
    PlanarBoundary,       // Boundary-constrained triangulation (planes)
    CylindricalStrip,     // Procedural cylinder strip
    ConicalStrip,         // Procedural cone strip
    SphericalGrid,        // Adaptive lat/lon grid
    ToroidalGrid,         // Adaptive torus grid
    ExtrusionStrip,       // Profile-based extrusion strip
    RevolutionRing,       // Profile-based revolution ring
    FreeformAdaptive,     // Curvature-driven OCC tessellation
    ProxyOmitted          // Face will be represented by proxy geometry (no mesh)
};

const char* tessellationStrategyName(TessellationStrategy s);

// ------------------------------------------------------------------
// Surface tag attached to each face/triangle
// ------------------------------------------------------------------
struct SurfaceTag {
    SurfaceCategory category = SurfaceCategory::Unknown;
    TessellationStrategy strategy = TessellationStrategy::FreeformAdaptive;
    float featureWeight = 1.0f;
    bool isPlanarRegion = false;
    bool isFeatureEdge = false;
    bool isCylindricalRegion = false;

    // Proxy geometry hint
    bool hasProxyCandidate = false;
    uint8_t proxyType = 0;  // index into ProxyGeometry type enum
};

// ------------------------------------------------------------------
// Classified face with tessellation strategy
// ------------------------------------------------------------------
struct ClassifiedFace {
    // Reference to original analysis
    int faceIndex = -1;
    const FaceAnalysis* analysis = nullptr;

    // Classification result
    SurfaceCategory category = SurfaceCategory::Unknown;
    TessellationStrategy tessStrategy = TessellationStrategy::FreeformAdaptive;

    // Tessellation params
    uint32_t recommendedSlices = 16;
    uint32_t recommendedStacks = 4;
    float maxEdgeLength = 1.0f;

    // Grouping: faces that should be tessellated together
    int planarGroupId = -1;  // coplanar faces share the same group

    // Surface tag for downstream pipeline
    SurfaceTag tag;
};

// ------------------------------------------------------------------
// Per-shape classification result
// ------------------------------------------------------------------
struct ClassificationResult {
    std::string shapeKey;
    std::vector<ClassifiedFace> classifiedFaces;

    // Strategy summary
    int planarFaceCount = 0;
    int cylinderFaceCount = 0;
    int coneFaceCount = 0;
    int sphereFaceCount = 0;
    int torusFaceCount = 0;
    int extrusionFaceCount = 0;
    int revolutionFaceCount = 0;
    int freeformFaceCount = 0;
    int proxyOmittedCount = 0;

    // Planar groups (coplanar faces)
    struct PlanarGroup {
        std::vector<int> faceIndices;
        Vec3 normal;
        float offset;
        float totalArea;
    };
    std::vector<PlanarGroup> planarGroups;
};

// ------------------------------------------------------------------
// Surface classifier
// ------------------------------------------------------------------
class SurfaceClassifier {
public:
    SurfaceClassifier();
    ~SurfaceClassifier();

    // Classify a single shape analysis
    ClassificationResult classify(const ShapeAnalysis& analysis);

    // Batch classify
    std::unordered_map<std::string, ClassificationResult>
    classifyBatch(const std::unordered_map<std::string, ShapeAnalysis>& analyses);

    // Configuration
    void setPlanarNormalTolerance(float degrees) { m_planarNormalTol = degrees; }
    void setPlanarOffsetTolerance(float dist) { m_planarOffsetTol = dist; }
    void setCoplanarAreaThreshold(float area) { m_coplanarAreaThreshold = area; }

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    float m_planarNormalTol = 1.0f;    // degrees
    float m_planarOffsetTol = 0.1f;    // mm
    float m_coplanarAreaThreshold = 10.0f; // mm², minimum area to form a group
};

} // namespace mf
