#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include "Geometry/SurfaceClassifier.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace mf {

// ------------------------------------------------------------------
// Proxy geometry type
// ------------------------------------------------------------------
enum class ProxyType : uint8_t {
    None,
    Plane,
    Cylinder,
    Cone,
    Sphere,
    Torus,
    Box,
    Pipe
};

// ------------------------------------------------------------------
// Parameterized proxy geometry descriptor
// ------------------------------------------------------------------
struct ProxyGeometry {
    ProxyType type = ProxyType::None;

    // Parameters (semantics depend on type)
    // Plane:   origin(0-2), normal(4-6), halfExtents(8-9)
    // Cylinder: baseCenter(0-2), axis(4-6), radius(8), height(9)
    // Cone:    apex(0-2), axis(4-6), radius(8), height(9), angle(10)
    // Sphere:  center(0-2), radius(8)
    // Torus:   center(0-2), axis(4-6), majorR(8), minorR(9)
    // Box:     bmin(0-2), bmax(4-6)
    float params[12]{};

    // Procedural generation hints
    uint32_t recommendedSlices = 16;
    uint32_t recommendedStacks = 4;

    // Generate a low-poly procedural mesh from parameters
    MeshData generateMesh() const;
};

// ------------------------------------------------------------------
// Recognized primitive group (set of faces replaced by proxy)
// ------------------------------------------------------------------
struct PrimitiveGroup {
    std::string groupId;
    ProxyGeometry proxy;
    ProxyType type = ProxyType::None;

    // Fit quality
    float maxDeviation = 0.0f;
    float meanDeviation = 0.0f;
    bool isExact = false;  // true if OCC type exactly matches

    // Associated face indices (across potentially multiple shapes)
    std::vector<std::string> shapeKeys;
    std::vector<int> faceIndices;

    // Corresponding classification faces
    std::vector<const ClassifiedFace*> classifiedFaces;
};

// ------------------------------------------------------------------
// Recognition result
// ------------------------------------------------------------------
struct PrimitiveRecognitionResult {
    std::vector<PrimitiveGroup> groups;

    // Statistics
    int planeCount = 0, cylinderCount = 0, coneCount = 0;
    int sphereCount = 0, torusCount = 0, boxCount = 0, pipeCount = 0;
    int totalFacesReplaced = 0;
};

// ------------------------------------------------------------------
// Primitive recognizer
// ------------------------------------------------------------------
class PrimitiveRecognizer {
public:
    PrimitiveRecognizer();
    ~PrimitiveRecognizer();

    // Recognize primitives from classification results
    PrimitiveRecognitionResult recognize(
        const std::unordered_map<std::string, ClassificationResult>& classifications,
        const std::unordered_map<std::string, ShapeAnalysis>& analyses);

    // Configuration
    void setPlaneMergeAngle(float deg) { m_planeMergeAngle = deg; }
    void setBoxAngleTolerance(float deg) { m_boxAngleTol = deg; }
    void setPipeAxisTolerance(float deg) { m_pipeAxisTol = deg; }
    void setMinFaceArea(float area) { m_minFaceArea = area; }

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    float m_planeMergeAngle = 1.0f;
    float m_boxAngleTol = 2.0f;
    float m_pipeAxisTol = 5.0f;
    float m_minFaceArea = 5.0f;
};

} // namespace mf
