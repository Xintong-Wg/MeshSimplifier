#pragma once

#include "Mesh/MeshData.h"
#include "Geometry/SurfaceClassifier.h"
#include <memory>
#include <vector>

class TopoDS_Shape;
class TopLoc_Location;

namespace mf {

// ------------------------------------------------------------------
// Adaptive tessellation parameters
// ------------------------------------------------------------------
struct AdaptiveTessellationParams {
    // Planar strategy
    float planarMaxEdgeLength = 50.0f;
    bool planarUseQuads = true;

    // Cylinder strategy
    uint32_t cylinderMinSlices = 8;
    uint32_t cylinderMaxSlices = 64;
    uint32_t cylinderAxialSegments = 2;

    // Cone strategy
    uint32_t coneMinSlices = 8;
    uint32_t coneMaxSlices = 64;
    uint32_t coneAxialSegments = 4;

    // Sphere strategy
    uint32_t sphereMinSlices = 8;
    uint32_t sphereMaxSlices = 32;

    // Torus strategy
    uint32_t torusMinSlices = 12;
    uint32_t torusMaxSlices = 48;

    // Freeform fallback (OCC BRepMesh parameters)
    float freeformLinearDeflection = 0.5f;
    float freeformAngularDeflection = 0.5f;

    // Global
    bool relativeToBBox = true;
    float minEdgeLength = 0.01f;
    bool parallelMeshing = true;    // let OpenCASCADE parallelize inside one shape

    // If classification is not available, fall back to uniform tessellation
    bool useClassification = true;
};

// ------------------------------------------------------------------
// Tessellated mesh with per-triangle surface tags
// ------------------------------------------------------------------
struct TaggedMesh {
    MeshData mesh;
    std::vector<SurfaceTag> triangleTags;  // 1:1 with triangles
};

// ------------------------------------------------------------------
// Adaptive tessellator
// ------------------------------------------------------------------
class AdaptiveTessellator {
public:
    AdaptiveTessellator();
    ~AdaptiveTessellator();

    // Tessellate with classification result (preferred path)
    TaggedMesh tessellate(const TopoDS_Shape& shape,
                           const ClassificationResult& classification,
                           const AdaptiveTessellationParams& params);

    // Tessellate with transform
    TaggedMesh tessellate(const TopoDS_Shape& shape,
                           const TopLoc_Location& loc,
                           const ClassificationResult& classification,
                           const AdaptiveTessellationParams& params);

    // Tessellate without classification (fallback uniform tessellation)
    MeshData tessellateUniform(const TopoDS_Shape& shape,
                                const AdaptiveTessellationParams& params);

    // Tessellate with transform, no classification
    MeshData tessellateUniform(const TopoDS_Shape& shape,
                                const TopLoc_Location& loc,
                                const AdaptiveTessellationParams& params);

    // Configuration
    void setSamplesPerFace(int n) { m_samplesPerFace = n; }

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    int m_samplesPerFace = 64;
};

} // namespace mf
