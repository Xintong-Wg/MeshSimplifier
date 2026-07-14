#include "Geometry/AdaptiveTessellator.h"
#include "Geometry/BRepAnalyzer.h"
#include "Core/Logger.h"

// OpenCASCADE tessellation
#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/BRep_Tool.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_ShapeEnum.hxx>
#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/Poly_Array1OfTriangle.hxx>
#include <opencascade/TColgp_Array1OfPnt.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Vec.hxx>
#include <opencascade/gp_Dir.hxx>
#include <opencascade/gp_Ax1.hxx>
#include <opencascade/gp_Ax3.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Circ.hxx>
#include <opencascade/BRepGProp.hxx>
#include <opencascade/GProp_GProps.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/TopoDS_Wire.hxx>
#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Bnd_Box.hxx>
#include <opencascade/BRepAdaptor_Surface.hxx>
#include <opencascade/GeomLProp_SLProps.hxx>
#include <opencascade/Geom_Surface.hxx>
#include <opencascade/Geom_Plane.hxx>
#include <opencascade/Geom_CylindricalSurface.hxx>
#include <opencascade/Geom_ConicalSurface.hxx>
#include <opencascade/Geom_SphericalSurface.hxx>
#include <opencascade/Geom_ToroidalSurface.hxx>
#include <opencascade/Geom_Circle.hxx>
#include <opencascade/Geom_Line.hxx>
#include <opencascade/TopExp.hxx>
#include <opencascade/TopTools_IndexedMapOfShape.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/BRepTools.hxx>
#include <opencascade/Standard_Real.hxx>
#include <opencascade/Standard_Failure.hxx>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <cstring>

namespace mf {

// ------------------------------------------------------------------
// Helper: compute surface normal at UV for a face
// ------------------------------------------------------------------
static Vec3 computeFaceNormal(const TopoDS_Face& face, const Handle(Poly_Triangulation)& tri,
                               int nodeIndex, const gp_Pnt2d& uv) {
    try {
        BRepAdaptor_Surface surf(face, Standard_True);  // restrict to face boundaries
        GeomLProp_SLProps props(surf.Surface().Surface(), uv.X(), uv.Y(), 1, 1e-6);
        if (props.IsNormalDefined()) {
            gp_Dir n = props.Normal();
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            return Vec3(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
        }
    } catch (...) {}

    // Fallback: average adjacent triangle normals
    Standard_Integer nbNodes = tri->NbNodes();
    if (nodeIndex >= 1 && nodeIndex <= nbNodes) {
        gp_Vec avg(0, 0, 0);
        int count = 0;
        for (Standard_Integer t = 1; t <= tri->NbTriangles(); ++t) {
            const Poly_Triangle& triangle = tri->Triangle(t);
            Standard_Integer n1, n2, n3;
            triangle.Get(n1, n2, n3);
            if (n1 == nodeIndex || n2 == nodeIndex || n3 == nodeIndex) {
                gp_Pnt p1 = tri->Node(n1), p2 = tri->Node(n2), p3 = tri->Node(n3);
                gp_Vec v1(p2.XYZ() - p1.XYZ()), v2(p3.XYZ() - p1.XYZ());
                gp_Vec fn = v1.Crossed(v2);
                if (fn.Magnitude() > 1e-12) {
                    avg += fn.Normalized();
                    ++count;
                }
            }
        }
        if (count > 0) {
            avg /= count;
            if (avg.Magnitude() > 1e-12) {
                avg.Normalize();
                return Vec3(static_cast<float>(avg.X()), static_cast<float>(avg.Y()), static_cast<float>(avg.Z()));
            }
        }
    }
    return Vec3(0, 0, 1);
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class AdaptiveTessellator::Impl {
public:
    // Per-category tessellation
    TaggedMesh tessellateOne(const TopoDS_Shape& shape,
                              const TopLoc_Location& loc,
                              const ClassificationResult& classification,
                              const AdaptiveTessellationParams& params);

    // Planar tessellation: use OCC with large deflection + quad merge
    void tessellatePlanarFace(const TopoDS_Face& face, const FaceAnalysis& fa,
                               const ClassifiedFace& cf, const TopLoc_Location& loc,
                               const AdaptiveTessellationParams& params,
                               std::vector<Vertex>& verts, std::vector<Index>& indices,
                               std::vector<SurfaceTag>& tags, uint32_t& vertOff);

    // Cylindrical procedural
    void generateCylinder(const TopoDS_Face& face, const FaceAnalysis& fa,
                           const ClassifiedFace& cf, const TopLoc_Location& loc,
                           const AdaptiveTessellationParams& params,
                           std::vector<Vertex>& verts, std::vector<Index>& indices,
                           std::vector<SurfaceTag>& tags, uint32_t& vertOff);

    // Conical procedural
    void generateCone(const TopoDS_Face& face, const FaceAnalysis& fa,
                       const ClassifiedFace& cf, const TopLoc_Location& loc,
                       const AdaptiveTessellationParams& params,
                       std::vector<Vertex>& verts, std::vector<Index>& indices,
                       std::vector<SurfaceTag>& tags, uint32_t& vertOff);

    // Spherical procedural
    void generateSphere(const TopoDS_Face& face, const FaceAnalysis& fa,
                         const ClassifiedFace& cf, const TopLoc_Location& loc,
                         const AdaptiveTessellationParams& params,
                         std::vector<Vertex>& verts, std::vector<Index>& indices,
                         std::vector<SurfaceTag>& tags, uint32_t& vertOff);

    // Freeform fallback (OCC BRepMesh)
    void tessellateFreeformFace(const TopoDS_Face& face, const FaceAnalysis& fa,
                                 const TopLoc_Location& loc,
                                 const AdaptiveTessellationParams& params,
                                 std::vector<Vertex>& verts, std::vector<Index>& indices,
                                 std::vector<SurfaceTag>& tags, uint32_t& vertOff);

    // Quad merging for planar faces
    void mergePlanarQuads(std::vector<Vertex>& verts, std::vector<Index>& indices);

    // Uniform tessellation (no classification)
    MeshData tessellateUniformImpl(const TopoDS_Shape& shape, const TopLoc_Location& loc,
                                    const AdaptiveTessellationParams& params);

    // Build a SurfaceTag for a category
    SurfaceTag makeTag(SurfaceCategory cat, TessellationStrategy strat);
};

AdaptiveTessellator::AdaptiveTessellator() : m_impl(std::make_unique<Impl>()) {}
AdaptiveTessellator::~AdaptiveTessellator() = default;

TaggedMesh AdaptiveTessellator::tessellate(const TopoDS_Shape& shape,
                                            const ClassificationResult& classification,
                                            const AdaptiveTessellationParams& params) {
    return m_impl->tessellateOne(shape, TopLoc_Location(), classification, params);
}

TaggedMesh AdaptiveTessellator::tessellate(const TopoDS_Shape& shape,
                                            const TopLoc_Location& loc,
                                            const ClassificationResult& classification,
                                            const AdaptiveTessellationParams& params) {
    return m_impl->tessellateOne(shape, loc, classification, params);
}

MeshData AdaptiveTessellator::tessellateUniform(const TopoDS_Shape& shape,
                                                  const AdaptiveTessellationParams& params) {
    return m_impl->tessellateUniformImpl(shape, TopLoc_Location(), params);
}

MeshData AdaptiveTessellator::tessellateUniform(const TopoDS_Shape& shape,
                                                  const TopLoc_Location& loc,
                                                  const AdaptiveTessellationParams& params) {
    return m_impl->tessellateUniformImpl(shape, loc, params);
}

// ------------------------------------------------------------------
SurfaceTag AdaptiveTessellator::Impl::makeTag(SurfaceCategory cat, TessellationStrategy strat) {
    SurfaceTag tag;
    tag.category = cat;
    tag.strategy = strat;
    tag.isPlanarRegion = (cat == SurfaceCategory::Planar);
    tag.isCylindricalRegion = (cat == SurfaceCategory::Cylindrical);
    switch (cat) {
        case SurfaceCategory::Planar:      tag.featureWeight = 5.0f; break;
        case SurfaceCategory::Cylindrical:
        case SurfaceCategory::Conical:     tag.featureWeight = 3.0f; break;
        case SurfaceCategory::Spherical:
        case SurfaceCategory::Toroidal:    tag.featureWeight = 2.0f; break;
        default:                           tag.featureWeight = 1.0f; break;
    }
    return tag;
}

// ------------------------------------------------------------------
// Main tessellation dispatch
// ------------------------------------------------------------------
TaggedMesh AdaptiveTessellator::Impl::tessellateOne(
    const TopoDS_Shape& shape, const TopLoc_Location& loc,
    const ClassificationResult& classification,
    const AdaptiveTessellationParams& params) {

    TaggedMesh result;
    std::vector<Vertex>& verts = result.mesh.vertices;
    std::vector<Index>& indices = result.mesh.indices;
    std::vector<SurfaceTag>& tags = result.triangleTags;
    uint32_t vertOff = 0;

    // Build face index → ClassifiedFace lookup
    std::unordered_map<int, const ClassifiedFace*> faceMap;
    for (auto& cf : classification.classifiedFaces) {
        faceMap[cf.faceIndex] = &cf;
    }

    // Index faces in shape
    TopTools_IndexedMapOfShape faceIdxMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceIdxMap);

    // Iterate faces by index matching classification
    for (int fi = 0; fi < static_cast<int>(classification.classifiedFaces.size()); ++fi) {
        const auto& cf = classification.classifiedFaces[fi];
        if (fi + 1 > faceIdxMap.Extent()) break;

        const TopoDS_Face& face = TopoDS::Face(faceIdxMap.FindKey(fi + 1));
        if (face.IsNull()) continue;

        const FaceAnalysis* fa = cf.analysis;
        FaceAnalysis defaultFa;
        if (!fa) { defaultFa.category = SurfaceCategory::Unknown; fa = &defaultFa; }

        TopLoc_Location faceLoc;
        Handle(Poly_Triangulation) existingTri = BRep_Tool::Triangulation(face, faceLoc);
        TopLoc_Location totalLoc = loc * faceLoc;

        // Procedural tessellation may throw if OCC surface evaluation fails
        // (e.g. trimmed surface with out-of-range UV params). Fall back to
        // freeform OCC tessellation which has better error handling.
        try {
            switch (cf.tessStrategy) {
                case TessellationStrategy::PlanarBoundary:
                    tessellatePlanarFace(face, *fa, cf, totalLoc, params, verts, indices, tags, vertOff);
                    break;
                case TessellationStrategy::CylindricalStrip:
                    generateCylinder(face, *fa, cf, totalLoc, params, verts, indices, tags, vertOff);
                    break;
                case TessellationStrategy::ConicalStrip:
                    generateCone(face, *fa, cf, totalLoc, params, verts, indices, tags, vertOff);
                    break;
                case TessellationStrategy::SphericalGrid:
                    generateSphere(face, *fa, cf, totalLoc, params, verts, indices, tags, vertOff);
                    break;
                case TessellationStrategy::ProxyOmitted:
                    break;
                default:
                    tessellateFreeformFace(face, *fa, totalLoc, params, verts, indices, tags, vertOff);
                    break;
            }
        } catch (const Standard_Failure& e) {
            MF_WARN("Procedural tessellation failed for face {} ({}), falling back to freeform: {}",
                    fi, surfaceCategoryName(fa->category),
                    e.GetMessageString() ? e.GetMessageString() : "unknown");
            // Fall back to OCC freeform tessellation
            try {
                tessellateFreeformFace(face, *fa, totalLoc, params, verts, indices, tags, vertOff);
            } catch (...) {
                MF_WARN("Freeform fallback also failed for face {}, skipping", fi);
            }
        } catch (...) {
            MF_WARN("Procedural tessellation failed for face {} ({}), falling back to freeform",
                    fi, surfaceCategoryName(fa->category));
            try {
                tessellateFreeformFace(face, *fa, totalLoc, params, verts, indices, tags, vertOff);
            } catch (...) {
                MF_WARN("Freeform fallback also failed for face {}, skipping", fi);
            }
        }
    }

    if (!verts.empty()) {
        result.mesh.computeAABB();
    }

    MF_INFO("AdaptiveTessellator: {} vertices, {} triangles",
            result.mesh.vertexCount(), result.mesh.triangleCount());
    return result;
}

// ------------------------------------------------------------------
// Planar face tessellation
// ------------------------------------------------------------------
void AdaptiveTessellator::Impl::tessellatePlanarFace(
    const TopoDS_Face& face, const FaceAnalysis& fa, const ClassifiedFace& cf,
    const TopLoc_Location& loc, const AdaptiveTessellationParams& params,
    std::vector<Vertex>& verts, std::vector<Index>& indices,
    std::vector<SurfaceTag>& tags, uint32_t& vertOff) {

    // Use OCC BRepMesh with generous deflection for planar surfaces
    // Planar surfaces have zero curvature, so deflection only controls edge refinement
    float planarDeflection = cf.maxEdgeLength * 2.0f;
    BRepMesh_IncrementalMesh incMesh(face, planarDeflection, Standard_False, 0.5,
                                     params.parallelMeshing ? Standard_True : Standard_False);

    SurfaceTag tag = makeTag(SurfaceCategory::Planar, TessellationStrategy::PlanarBoundary);

    TopLoc_Location faceLoc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, faceLoc);
    if (tri.IsNull()) return;

    TopLoc_Location totalLoc = loc * faceLoc;
    gp_Trsf totalTrsf = totalLoc.Transformation();
    bool hasUV = tri->HasUVNodes();

    Standard_Integer nbNodes = tri->NbNodes();
    Standard_Integer nbTriangles = tri->NbTriangles();

    // Add vertices
    for (Standard_Integer n = 1; n <= nbNodes; ++n) {
        gp_Pnt p = tri->Node(n).Transformed(totalTrsf);
        Vertex v;
        v.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
        v.normal = fa.planeNormal;
        if (face.Orientation() == TopAbs_REVERSED) v.normal = -v.normal;
        // Rotate normal by transform (ignore translation)
        gp_Dir nDir(v.normal.x, v.normal.y, v.normal.z);
        nDir.Transform(totalTrsf);
        v.normal = Vec3(static_cast<float>(nDir.X()), static_cast<float>(nDir.Y()), static_cast<float>(nDir.Z()));
        v.normal = glm::normalize(v.normal);
        v.uv = Vec2(0, 0);
        if (hasUV) {
            gp_Pnt2d uvNode = tri->UVNode(n);
            v.uv = Vec2(static_cast<float>(uvNode.X()), static_cast<float>(uvNode.Y()));
        }
        v.tangent = Vec4(1, 0, 0, 1);
        verts.push_back(v);
    }

    // Add indices
    for (Standard_Integer t = 1; t <= nbTriangles; ++t) {
        const Poly_Triangle& triangle = tri->Triangle(t);
        Standard_Integer n1, n2, n3;
        triangle.Get(n1, n2, n3);
        indices.push_back(vertOff + static_cast<Index>(n1 - 1));
        indices.push_back(vertOff + static_cast<Index>(n2 - 1));
        indices.push_back(vertOff + static_cast<Index>(n3 - 1));
        tags.push_back(tag);
    }

    vertOff += static_cast<uint32_t>(nbNodes);

    // Quad merge for large planar faces
    if (params.planarUseQuads && nbTriangles > 4) {
        // Mark the index range for quad merging (done in post-process)
        // For now we keep triangles; a full constrained Delaunay + quad merge
        // would require integrating a library like poly2tri or Triangle.
        // The key optimization is the large deflection which already
        // dramatically reduces triangle count vs uniform tessellation.
    }
}

// ------------------------------------------------------------------
// Cylindrical procedural generation
// ------------------------------------------------------------------
void AdaptiveTessellator::Impl::generateCylinder(
    const TopoDS_Face& face, const FaceAnalysis& fa, const ClassifiedFace& cf,
    const TopLoc_Location& loc, const AdaptiveTessellationParams& params,
    std::vector<Vertex>& verts, std::vector<Index>& indices,
    std::vector<SurfaceTag>& tags, uint32_t& vertOff) {

    uint32_t slices = std::max(params.cylinderMinSlices,
                                std::min(params.cylinderMaxSlices, cf.recommendedSlices));
    uint32_t stacks = params.cylinderAxialSegments;

    gp_Trsf trsf = loc.Transformation();
    SurfaceTag tag = makeTag(SurfaceCategory::Cylindrical, TessellationStrategy::CylindricalStrip);

    // Use OCC's BRepAdaptor to get accurate cylinder parameters
    BRepAdaptor_Surface adaptor(face, Standard_True);  // restrict to face boundaries
    Handle(Geom_Surface) surf = adaptor.Surface().Surface();
    auto cylSurf = Handle(Geom_CylindricalSurface)::DownCast(surf);

    float uMin = static_cast<float>(adaptor.FirstUParameter());
    float uMax = static_cast<float>(adaptor.LastUParameter());
    float vMin = static_cast<float>(adaptor.FirstVParameter());
    float vMax = static_cast<float>(adaptor.LastVParameter());

    // Get cylinder axis, radius from face analysis
    Vec3 axis = fa.axisDirection;
    float radius = fa.radius;

    // If OCC provides exact cylinder, use its parameters
    if (!cylSurf.IsNull()) {
        gp_Cylinder gcyl = cylSurf->Cylinder();
        gp_Dir ax = gcyl.Axis().Direction();
        axis = Vec3(static_cast<float>(ax.X()), static_cast<float>(ax.Y()), static_cast<float>(ax.Z()));
        radius = static_cast<float>(gcyl.Radius());
    }
    (void)radius;  // reserved for future radius-based slice adaptation

    // Generate vertices: stacks+1 rings of slices vertices each
    for (uint32_t si = 0; si <= stacks; ++si) {
        float vFrac = static_cast<float>(si) / static_cast<float>(stacks);
        float z = vMin + vFrac * (vMax - vMin);

        for (uint32_t ci = 0; ci < slices; ++ci) {
            float uFrac = static_cast<float>(ci) / static_cast<float>(slices);
            float angle = uMin + uFrac * (uMax - uMin);

            // Evaluate surface point at (u, v)
            gp_Pnt pt;
            gp_Vec d1u, d1v;
            surf->D1(angle, z, pt, d1u, d1v);

            gp_Pnt p = pt.Transformed(trsf);
            gp_Dir n = gp_Dir(d1u.Crossed(d1v));
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            n.Transform(trsf);

            Vertex v;
            v.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            v.normal = Vec3(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
            v.normal = glm::normalize(v.normal);
            v.uv = Vec2(uFrac, vFrac);
            v.tangent = Vec4(1, 0, 0, 1);
            verts.push_back(v);
        }
    }

    // Generate indices (triangle strips → indexed triangles)
    for (uint32_t si = 0; si < stacks; ++si) {
        for (uint32_t ci = 0; ci < slices; ++ci) {
            uint32_t a = vertOff + si * slices + ci;
            uint32_t b = vertOff + si * slices + (ci + 1) % slices;
            uint32_t c = vertOff + (si + 1) * slices + ci;
            uint32_t d = vertOff + (si + 1) * slices + (ci + 1) % slices;

            // Two triangles per quad
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            tags.push_back(tag);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
            tags.push_back(tag);
        }
    }

    vertOff += (stacks + 1) * slices;
}

// ------------------------------------------------------------------
// Conical procedural generation
// ------------------------------------------------------------------
void AdaptiveTessellator::Impl::generateCone(
    const TopoDS_Face& face, const FaceAnalysis& fa, const ClassifiedFace& cf,
    const TopLoc_Location& loc, const AdaptiveTessellationParams& params,
    std::vector<Vertex>& verts, std::vector<Index>& indices,
    std::vector<SurfaceTag>& tags, uint32_t& vertOff) {

    uint32_t slices = std::max(params.coneMinSlices,
                                std::min(params.coneMaxSlices, cf.recommendedSlices));
    uint32_t stacks = std::max(params.coneAxialSegments, cf.recommendedStacks);

    gp_Trsf trsf = loc.Transformation();
    SurfaceTag tag = makeTag(SurfaceCategory::Conical, TessellationStrategy::ConicalStrip);

    BRepAdaptor_Surface adaptor(face, Standard_True);  // restrict to face boundaries
    Handle(Geom_Surface) surf = adaptor.Surface().Surface();

    float uMin = static_cast<float>(adaptor.FirstUParameter());
    float uMax = static_cast<float>(adaptor.LastUParameter());
    float vMin = static_cast<float>(adaptor.FirstVParameter());
    float vMax = static_cast<float>(adaptor.LastVParameter());

    // Generate vertices
    for (uint32_t si = 0; si <= stacks; ++si) {
        float vFrac = static_cast<float>(si) / static_cast<float>(stacks);
        float v = vMin + vFrac * (vMax - vMin);

        for (uint32_t ci = 0; ci < slices; ++ci) {
            float uFrac = static_cast<float>(ci) / static_cast<float>(slices);
            float u = uMin + uFrac * (uMax - uMin);

            gp_Pnt pt;
            gp_Vec d1u, d1v;
            surf->D1(u, v, pt, d1u, d1v);
            gp_Pnt p = pt.Transformed(trsf);
            gp_Dir n = gp_Dir(d1u.Crossed(d1v));
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            n.Transform(trsf);

            Vertex vtx;
            vtx.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            vtx.normal = Vec3(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
            vtx.normal = glm::normalize(vtx.normal);
            vtx.uv = Vec2(uFrac, vFrac);
            vtx.tangent = Vec4(1, 0, 0, 1);
            verts.push_back(vtx);
        }
    }

    // Generate indices
    for (uint32_t si = 0; si < stacks; ++si) {
        for (uint32_t ci = 0; ci < slices; ++ci) {
            uint32_t a = vertOff + si * slices + ci;
            uint32_t b = vertOff + si * slices + (ci + 1) % slices;
            uint32_t c = vertOff + (si + 1) * slices + ci;
            uint32_t d = vertOff + (si + 1) * slices + (ci + 1) % slices;

            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            tags.push_back(tag);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
            tags.push_back(tag);
        }
    }

    vertOff += (stacks + 1) * slices;
}

// ------------------------------------------------------------------
// Spherical procedural generation
// ------------------------------------------------------------------
void AdaptiveTessellator::Impl::generateSphere(
    const TopoDS_Face& face, const FaceAnalysis& fa, const ClassifiedFace& cf,
    const TopLoc_Location& loc, const AdaptiveTessellationParams& params,
    std::vector<Vertex>& verts, std::vector<Index>& indices,
    std::vector<SurfaceTag>& tags, uint32_t& vertOff) {

    uint32_t slices = std::max(params.sphereMinSlices,
                                std::min(params.sphereMaxSlices, cf.recommendedSlices));
    uint32_t stacks = std::max(4u, slices / 2);

    gp_Trsf trsf = loc.Transformation();
    SurfaceTag tag = makeTag(SurfaceCategory::Spherical, TessellationStrategy::SphericalGrid);

    BRepAdaptor_Surface adaptor(face, Standard_True);  // restrict to face boundaries
    Handle(Geom_Surface) surf = adaptor.Surface().Surface();

    float uMin = static_cast<float>(adaptor.FirstUParameter());
    float uMax = static_cast<float>(adaptor.LastUParameter());
    float vMin = static_cast<float>(adaptor.FirstVParameter());
    float vMax = static_cast<float>(adaptor.LastVParameter());

    for (uint32_t si = 0; si <= stacks; ++si) {
        float vFrac = static_cast<float>(si) / static_cast<float>(stacks);
        float v = vMin + vFrac * (vMax - vMin);

        for (uint32_t ci = 0; ci < slices; ++ci) {
            float uFrac = static_cast<float>(ci) / static_cast<float>(slices);
            float u = uMin + uFrac * (uMax - uMin);

            gp_Pnt pt;
            gp_Vec d1u, d1v;
            surf->D1(u, v, pt, d1u, d1v);
            gp_Pnt p = pt.Transformed(trsf);
            gp_Dir n = gp_Dir(d1u.Crossed(d1v));
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            n.Transform(trsf);

            Vertex vtx;
            vtx.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            vtx.normal = Vec3(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
            vtx.normal = glm::normalize(vtx.normal);
            vtx.uv = Vec2(uFrac, vFrac);
            vtx.tangent = Vec4(1, 0, 0, 1);
            verts.push_back(vtx);
        }
    }

    for (uint32_t si = 0; si < stacks; ++si) {
        for (uint32_t ci = 0; ci < slices; ++ci) {
            uint32_t a = vertOff + si * slices + ci;
            uint32_t b = vertOff + si * slices + (ci + 1) % slices;
            uint32_t c = vertOff + (si + 1) * slices + ci;
            uint32_t d = vertOff + (si + 1) * slices + (ci + 1) % slices;

            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            tags.push_back(tag);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
            tags.push_back(tag);
        }
    }

    vertOff += (stacks + 1) * slices;
}

// ------------------------------------------------------------------
// Freeform fallback: OCC BRepMesh with curvature-driven deflection
// ------------------------------------------------------------------
void AdaptiveTessellator::Impl::tessellateFreeformFace(
    const TopoDS_Face& face, const FaceAnalysis& fa, const TopLoc_Location& loc,
    const AdaptiveTessellationParams& params,
    std::vector<Vertex>& verts, std::vector<Index>& indices,
    std::vector<SurfaceTag>& tags, uint32_t& vertOff) {

    // Adjust deflection based on curvature
    float linDefl = params.freeformLinearDeflection;
    float rmsCurv = fa.rmsCurvature;
    if (rmsCurv > 0.001f) {
        // Higher curvature → finer tessellation
        linDefl = std::max(0.05f, linDefl / (1.0f + rmsCurv * 10.0f));
    }

    BRepMesh_IncrementalMesh incMesh(face, linDefl, Standard_False,
                                      params.freeformAngularDeflection,
                                      params.parallelMeshing ? Standard_True : Standard_False);

    SurfaceTag tag = makeTag(fa.category, TessellationStrategy::FreeformAdaptive);

    TopLoc_Location faceLoc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, faceLoc);
    if (tri.IsNull()) return;

    TopLoc_Location totalLoc = loc * faceLoc;
    gp_Trsf totalTrsf = totalLoc.Transformation();
    bool hasUV = tri->HasUVNodes();

    Standard_Integer nbNodes = tri->NbNodes();

    for (Standard_Integer n = 1; n <= nbNodes; ++n) {
        gp_Pnt p = tri->Node(n).Transformed(totalTrsf);
        Vertex v;
        v.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));

        if (hasUV) {
            gp_Pnt2d uvNode = tri->UVNode(n);
            v.uv = Vec2(static_cast<float>(uvNode.X()), static_cast<float>(uvNode.Y()));
            v.normal = computeFaceNormal(face, tri, n, uvNode);
        } else {
            v.uv = Vec2(0, 0);
            v.normal = Vec3(0, 0, 1);
        }

        // Transform normal
        gp_Dir nDir(v.normal.x, v.normal.y, v.normal.z);
        nDir.Transform(totalTrsf);
        v.normal = Vec3(static_cast<float>(nDir.X()), static_cast<float>(nDir.Y()), static_cast<float>(nDir.Z()));
        v.normal = glm::normalize(v.normal);
        v.tangent = Vec4(1, 0, 0, 1);
        verts.push_back(v);
    }

    for (Standard_Integer t = 1; t <= tri->NbTriangles(); ++t) {
        const Poly_Triangle& triangle = tri->Triangle(t);
        Standard_Integer n1, n2, n3;
        triangle.Get(n1, n2, n3);
        indices.push_back(vertOff + static_cast<Index>(n1 - 1));
        indices.push_back(vertOff + static_cast<Index>(n2 - 1));
        indices.push_back(vertOff + static_cast<Index>(n3 - 1));
        tags.push_back(tag);
    }

    vertOff += static_cast<uint32_t>(nbNodes);
}

// ------------------------------------------------------------------
// Uniform tessellation (no classification available)
// ------------------------------------------------------------------
MeshData AdaptiveTessellator::Impl::tessellateUniformImpl(
    const TopoDS_Shape& shape, const TopLoc_Location& loc,
    const AdaptiveTessellationParams& params) {

    MeshData mesh;
    if (shape.IsNull()) return mesh;

    float linDefl = params.freeformLinearDeflection;

    if (params.relativeToBBox) {
        GProp_GProps gprops;
        BRepGProp::VolumeProperties(shape, gprops);
        if (gprops.Mass() > 0) {
            Bnd_Box box;
            BRepBndLib::Add(shape, box);
            if (!box.IsVoid()) {
                float diag = static_cast<float>(box.SquareExtent());
                linDefl = std::max(diag * params.freeformLinearDeflection, params.minEdgeLength);
            }
        }
    }

    BRepMesh_IncrementalMesh incMesh(shape, linDefl, Standard_False,
                                      params.freeformAngularDeflection,
                                      params.parallelMeshing ? Standard_True : Standard_False);

    uint32_t vertOff = 0;
    TopExp_Explorer faceExp(shape, TopAbs_FACE);

    for (; faceExp.More(); faceExp.Next()) {
        TopoDS_Face face = TopoDS::Face(faceExp.Current());
        TopLoc_Location faceLoc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, faceLoc);
        if (tri.IsNull()) continue;

        TopLoc_Location totalLoc = loc * faceLoc;
        gp_Trsf totalTrsf = totalLoc.Transformation();
        bool hasUV = tri->HasUVNodes();
        Standard_Integer nbNodes = tri->NbNodes();

        for (Standard_Integer n = 1; n <= nbNodes; ++n) {
            gp_Pnt p = tri->Node(n).Transformed(totalTrsf);
            Vertex v;
            v.position = Vec3(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            v.normal = Vec3(0, 0, 1);
            if (hasUV) {
                gp_Pnt2d uvNode = tri->UVNode(n);
                v.uv = Vec2(static_cast<float>(uvNode.X()), static_cast<float>(uvNode.Y()));
                v.normal = computeFaceNormal(face, tri, n, uvNode);
            }
            gp_Dir nDir(v.normal.x, v.normal.y, v.normal.z);
            nDir.Transform(totalTrsf);
            v.normal = Vec3(static_cast<float>(nDir.X()), static_cast<float>(nDir.Y()), static_cast<float>(nDir.Z()));
            v.normal = glm::normalize(v.normal);
            v.tangent = Vec4(1, 0, 0, 1);
            mesh.vertices.push_back(v);
        }

        for (Standard_Integer t = 1; t <= tri->NbTriangles(); ++t) {
            const Poly_Triangle& triangle = tri->Triangle(t);
            Standard_Integer n1, n2, n3;
            triangle.Get(n1, n2, n3);
            mesh.indices.push_back(vertOff + static_cast<Index>(n1 - 1));
            mesh.indices.push_back(vertOff + static_cast<Index>(n2 - 1));
            mesh.indices.push_back(vertOff + static_cast<Index>(n3 - 1));
        }
        vertOff += static_cast<uint32_t>(nbNodes);
    }

    mesh.computeAABB();
    return mesh;
}

} // namespace mf
