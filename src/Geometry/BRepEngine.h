#pragma once

#include "Core/Types.h"
#include <memory>
#include <vector>

// Forward declarations for OpenCASCADE
typedef int Standard_Integer;
class TopoDS_Shape;
class TopLoc_Location;

namespace mf {

struct AssemblyNode;

// ------------------------------------------------------------------
// BRep meshing parameters
// ------------------------------------------------------------------
struct TessellationParams {
    float linearDeflection = 0.5f;   // chordal deviation (mm)
    float angularDeflection = 0.5f;  // angular deviation (rad)
    bool  relative = true;           // relative to bounding box
    float minEdgeLength = 0.01f;     // ignore edges shorter than this
    bool  parallelMeshing = true;    // let OpenCASCADE parallelize inside one shape
};

// ------------------------------------------------------------------
// Raw mesh extracted from a single BRep shape
// ------------------------------------------------------------------
struct BRepMesh {
    std::vector<Vertex> vertices;
    std::vector<Index>  indices;
    AABB aabb;
    std::string materialName;
    uint32_t faceCount = 0;
    uint32_t triangleCount = 0;
};

// ------------------------------------------------------------------
// BRep tessellation engine
// ------------------------------------------------------------------
class BRepEngine {
public:
    BRepEngine();
    ~BRepEngine();

    // Tessellate a shape into raw mesh
    BRepMesh tessellate(const TopoDS_Shape& shape, const TessellationParams& params);

    // Tessellate with location (for instances)
    BRepMesh tessellate(const TopoDS_Shape& shape, const TopLoc_Location& loc,
                        const TessellationParams& params);

    // Tessellate with glm transform matrix (convenience)
    BRepMesh tessellate(const TopoDS_Shape& shape, const Mat4& transform,
                        const TessellationParams& params);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mf
