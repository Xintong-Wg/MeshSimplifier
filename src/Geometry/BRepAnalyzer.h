#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

class TopoDS_Shape;

namespace mf {

// ------------------------------------------------------------------
// Surface category enumeration (shared across pipeline)
// ------------------------------------------------------------------
enum class SurfaceCategory : uint8_t {
    Unknown,
    Planar,
    Cylindrical,
    Conical,
    Spherical,
    Toroidal,
    Extrusion,
    Revolution,
    Freeform
};

const char* surfaceCategoryName(SurfaceCategory cat);

// ------------------------------------------------------------------
// Per-face geometric analysis result
// ------------------------------------------------------------------
struct FaceAnalysis {
    int faceIndex = -1;
    SurfaceCategory category = SurfaceCategory::Unknown;

    // Curvature statistics (sampled across UV domain)
    float minGaussCurvature = 0.0f;
    float maxGaussCurvature = 0.0f;
    float meanCurvature = 0.0f;
    float rmsCurvature = 0.0f;

    // Geometric extents
    float area = 0.0f;
    float perimeter = 0.0f;
    AABB localAABB;

    // Parametric domain info
    bool isPeriodicU = false;
    bool isPeriodicV = false;
    float uMin = 0.0f, uMax = 0.0f;
    float vMin = 0.0f, vMax = 0.0f;

    // --- Surface-type-specific attributes ---

    // Planar
    Vec3 planeNormal = Vec3(0, 0, 1);
    float planeOffset = 0.0f;

    // Cylindrical / Conical
    Vec3 axisDirection = Vec3(0, 0, 1);
    float radius = 0.0f;
    float height = 0.0f;
    float coneAngle = 0.0f;  // half-angle for cones

    // Spherical
    Vec3 sphereCenter = Vec3(0, 0, 0);
    float sphereRadius = 0.0f;

    // Toroidal
    float majorRadius = 0.0f;
    float minorRadius = 0.0f;

    // Topology
    std::vector<int> neighborFaces;
    bool isOnOuterBoundary = true;

    // Original OCC surface type (for debugging)
    int occSurfaceType = -1;  // GeomAbs_SurfaceType cast to int
};

// ------------------------------------------------------------------
// Per-shape analysis result (collection of faces)
// ------------------------------------------------------------------
struct ShapeAnalysis {
    std::string shapeKey;
    std::vector<FaceAnalysis> faces;
    AABB bbox;
    float totalSurfaceArea = 0.0f;

    // Counts by category
    int planarCount = 0;
    int cylindricalCount = 0;
    int conicalCount = 0;
    int sphericalCount = 0;
    int toroidalCount = 0;
    int extrusionCount = 0;
    int revolutionCount = 0;
    int freeformCount = 0;

    // Summary
    bool hasAnalyticalSurfaces() const {
        return planarCount + cylindricalCount + conicalCount +
               sphericalCount + toroidalCount > 0;
    }

    float analyticalFraction() const {
        int analytical = planarCount + cylindricalCount + conicalCount +
                         sphericalCount + toroidalCount;
        int total = static_cast<int>(faces.size());
        return total > 0 ? static_cast<float>(analytical) / static_cast<float>(total) : 0.0f;
    }
};

// ------------------------------------------------------------------
// BRep geometry analyzer
// ------------------------------------------------------------------
class BRepAnalyzer {
public:
    BRepAnalyzer();
    ~BRepAnalyzer();

    // Analyze a single shape (typically a Part solid)
    ShapeAnalysis analyze(const TopoDS_Shape& shape, const std::string& shapeKey);

    // Batch analyze multiple shapes (parallel via TBB)
    std::unordered_map<std::string, ShapeAnalysis>
    analyzeBatch(const std::unordered_map<std::string, TopoDS_Shape>& shapesByKey);

    // Sampling density for curvature estimation
    void setSamplesPerFace(int n) { m_samplesPerFace = n; }
    int samplesPerFace() const { return m_samplesPerFace; }

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    int m_samplesPerFace = 64;  // default: 8x8 UV samples
};

} // namespace mf
