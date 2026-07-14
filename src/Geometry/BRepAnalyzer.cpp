#include "Geometry/BRepAnalyzer.h"
#include "Core/Logger.h"

#include <opencascade/BRepAdaptor_Surface.hxx>
#include <opencascade/GeomLProp_SLProps.hxx>
#include <opencascade/GeomAdaptor_Surface.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_ShapeEnum.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/TopoDS_Edge.hxx>
#include <opencascade/TopExp.hxx>
#include <opencascade/BRepGProp.hxx>
#include <opencascade/GProp_GProps.hxx>
#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Bnd_Box.hxx>
#include <opencascade/Geom_Surface.hxx>
#include <opencascade/Geom_Plane.hxx>
#include <opencascade/Geom_CylindricalSurface.hxx>
#include <opencascade/Geom_ConicalSurface.hxx>
#include <opencascade/Geom_SphericalSurface.hxx>
#include <opencascade/Geom_ToroidalSurface.hxx>
#include <opencascade/Geom_Curve.hxx>
#include <opencascade/Geom_Line.hxx>
#include <opencascade/Geom_Circle.hxx>
#include <opencascade/Geom_BSplineSurface.hxx>
#include <opencascade/Geom_BezierSurface.hxx>
#include <opencascade/Geom_SurfaceOfLinearExtrusion.hxx>
#include <opencascade/Geom_SurfaceOfRevolution.hxx>
#include <opencascade/gp_Pln.hxx>
#include <opencascade/gp_Cylinder.hxx>
#include <opencascade/gp_Cone.hxx>
#include <opencascade/gp_Sphere.hxx>
#include <opencascade/gp_Torus.hxx>
#include <opencascade/gp_Ax1.hxx>
#include <opencascade/gp_Ax3.hxx>
#include <opencascade/gp_Dir.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Vec.hxx>
#include <opencascade/GeomAbs_SurfaceType.hxx>
#include <opencascade/GeomAbs_CurveType.hxx>
#include <opencascade/Standard_Real.hxx>
#include <opencascade/Standard_Integer.hxx>
#include <opencascade/TopTools_IndexedMapOfShape.hxx>
#include <opencascade/TopTools_IndexedDataMapOfShapeListOfShape.hxx>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <cmath>
#include <algorithm>
#include <limits>

namespace mf {

const char* surfaceCategoryName(SurfaceCategory cat) {
    switch (cat) {
        case SurfaceCategory::Planar:      return "Planar";
        case SurfaceCategory::Cylindrical: return "Cylindrical";
        case SurfaceCategory::Conical:     return "Conical";
        case SurfaceCategory::Spherical:   return "Spherical";
        case SurfaceCategory::Toroidal:    return "Toroidal";
        case SurfaceCategory::Extrusion:   return "Extrusion";
        case SurfaceCategory::Revolution:  return "Revolution";
        case SurfaceCategory::Freeform:    return "Freeform";
        default:                           return "Unknown";
    }
}

// ------------------------------------------------------------------
// Helper: convert OCC surface type to our SurfaceCategory
// ------------------------------------------------------------------
static SurfaceCategory occToCategory(GeomAbs_SurfaceType st) {
    switch (st) {
        case GeomAbs_Plane:              return SurfaceCategory::Planar;
        case GeomAbs_Cylinder:           return SurfaceCategory::Cylindrical;
        case GeomAbs_Cone:               return SurfaceCategory::Conical;
        case GeomAbs_Sphere:             return SurfaceCategory::Spherical;
        case GeomAbs_Torus:              return SurfaceCategory::Toroidal;
        case GeomAbs_SurfaceOfExtrusion: return SurfaceCategory::Extrusion;
        case GeomAbs_SurfaceOfRevolution:return SurfaceCategory::Revolution;
        default:                         return SurfaceCategory::Freeform;
    }
}

// ------------------------------------------------------------------
// Helper: curvature-based reclassification
// Detects "fake planes" — surfaces OCC reports as Spline/Bezier
// but are geometrically flat
// ------------------------------------------------------------------
static constexpr float kPlanarCurvatureThreshold = 1e-9f;  // nearly zero curvature = plane
static constexpr float kCylindricalMaxGauss = 1e-9f;        // zero gauss + nonzero mean = cylinder
static constexpr float kMinMeanCurvatureForCylinder = 1e-8f;

static SurfaceCategory reclassifyByCurvature(SurfaceCategory occCat,
                                              float maxGauss, float meanCurv) {
    float absMaxGauss = std::abs(maxGauss);
    float absMean = std::abs(meanCurv);

    // If OCC says it's a plane (or extrusion/revolution could be planar)
    if (occCat != SurfaceCategory::Planar) {
        // Check if geometrically planar
        if (absMaxGauss < kPlanarCurvatureThreshold && absMean < kPlanarCurvatureThreshold) {
            return SurfaceCategory::Planar;
        }
    }

    // Check for "fake cylinder" — a BSpline that's actually cylindrical
    if (occCat == SurfaceCategory::Freeform) {
        if (absMaxGauss < kCylindricalMaxGauss && absMean > kMinMeanCurvatureForCylinder) {
            return SurfaceCategory::Cylindrical;
        }
    }

    return occCat;
}

// ------------------------------------------------------------------
// Helper: extract surface-specific parameters
// ------------------------------------------------------------------
static void extractSurfaceParams(const Handle(Geom_Surface)& surf,
                                  GeomAbs_SurfaceType occType,
                                  FaceAnalysis& fa) {
    switch (occType) {
        case GeomAbs_Plane: {
            Handle(Geom_Plane)::DownCast(surf);
            auto plane = Handle(Geom_Plane)::DownCast(surf);
            if (!plane.IsNull()) {
                gp_Pln pln = plane->Pln();
                gp_Dir dir = pln.Axis().Direction();
                fa.planeNormal = Vec3(static_cast<float>(dir.X()),
                                       static_cast<float>(dir.Y()),
                                       static_cast<float>(dir.Z()));
                gp_Pnt loc = pln.Location();
                fa.planeOffset = static_cast<float>(
                    dir.X() * loc.X() + dir.Y() * loc.Y() + dir.Z() * loc.Z());
            }
            break;
        }
        case GeomAbs_Cylinder: {
            auto cyl = Handle(Geom_CylindricalSurface)::DownCast(surf);
            if (!cyl.IsNull()) {
                gp_Cylinder gcyl = cyl->Cylinder();
                gp_Dir ax = gcyl.Axis().Direction();
                fa.axisDirection = Vec3(static_cast<float>(ax.X()),
                                         static_cast<float>(ax.Y()),
                                         static_cast<float>(ax.Z()));
                fa.radius = static_cast<float>(gcyl.Radius());
            }
            break;
        }
        case GeomAbs_Cone: {
            auto cone = Handle(Geom_ConicalSurface)::DownCast(surf);
            if (!cone.IsNull()) {
                gp_Cone gcone = cone->Cone();
                gp_Dir ax = gcone.Axis().Direction();
                fa.axisDirection = Vec3(static_cast<float>(ax.X()),
                                         static_cast<float>(ax.Y()),
                                         static_cast<float>(ax.Z()));
                fa.radius = static_cast<float>(gcone.RefRadius());
                fa.coneAngle = static_cast<float>(gcone.SemiAngle());
            }
            break;
        }
        case GeomAbs_Sphere: {
            auto sph = Handle(Geom_SphericalSurface)::DownCast(surf);
            if (!sph.IsNull()) {
                gp_Sphere gsph = sph->Sphere();
                gp_Pnt c = gsph.Location();
                fa.sphereCenter = Vec3(static_cast<float>(c.X()),
                                        static_cast<float>(c.Y()),
                                        static_cast<float>(c.Z()));
                fa.sphereRadius = static_cast<float>(gsph.Radius());
            }
            break;
        }
        case GeomAbs_Torus: {
            auto tor = Handle(Geom_ToroidalSurface)::DownCast(surf);
            if (!tor.IsNull()) {
                gp_Torus gtor = tor->Torus();
                gp_Dir ax = gtor.Axis().Direction();
                fa.axisDirection = Vec3(static_cast<float>(ax.X()),
                                         static_cast<float>(ax.Y()),
                                         static_cast<float>(ax.Z()));
                fa.majorRadius = static_cast<float>(gtor.MajorRadius());
                fa.minorRadius = static_cast<float>(gtor.MinorRadius());
            }
            break;
        }
        default:
            break;
    }
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class BRepAnalyzer::Impl {
public:
    int samplesPerFace = 64;  // sqrt(samples) per UV direction

    ShapeAnalysis analyzeOne(const TopoDS_Shape& shape, const std::string& shapeKey);
    FaceAnalysis analyzeFace(const TopoDS_Face& face, int faceIndex);
    void buildNeighborGraph(ShapeAnalysis& sa, const TopoDS_Shape& shape);
};

BRepAnalyzer::BRepAnalyzer() : m_impl(std::make_unique<Impl>()) {}
BRepAnalyzer::~BRepAnalyzer() = default;

ShapeAnalysis BRepAnalyzer::analyze(const TopoDS_Shape& shape, const std::string& shapeKey) {
    return m_impl->analyzeOne(shape, shapeKey);
}

std::unordered_map<std::string, ShapeAnalysis>
BRepAnalyzer::analyzeBatch(const std::unordered_map<std::string, TopoDS_Shape>& shapesByKey) {
    std::unordered_map<std::string, ShapeAnalysis> results;
    std::mutex resultsMutex;

    // Build a vector of [key, shape] pairs for TBB
    std::vector<std::pair<const std::string*, const TopoDS_Shape*>> items;
    items.reserve(shapesByKey.size());
    for (const auto& [key, shp] : shapesByKey) {
        items.emplace_back(&key, &shp);
    }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, items.size()),
        [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                auto [keyPtr, shpPtr] = items[i];
                auto analysis = m_impl->analyzeOne(*shpPtr, *keyPtr);
                std::lock_guard<std::mutex> lock(resultsMutex);
                results[*keyPtr] = std::move(analysis);
            }
        });

    return results;
}

// ------------------------------------------------------------------
// Analyze one face (performance-critical, called per face)
// ------------------------------------------------------------------
FaceAnalysis BRepAnalyzer::Impl::analyzeFace(const TopoDS_Face& face, int faceIndex) {
    FaceAnalysis fa;
    fa.faceIndex = faceIndex;

    BRepAdaptor_Surface adaptor(face, Standard_True);  // restrict to face trimming boundaries
    GeomAbs_SurfaceType occType = adaptor.GetType();
    fa.occSurfaceType = static_cast<int>(occType);

    Handle(Geom_Surface) surf = adaptor.Surface().Surface();
    if (surf.IsNull()) return fa;

    // Parameter range
    fa.uMin = static_cast<float>(adaptor.FirstUParameter());
    fa.uMax = static_cast<float>(adaptor.LastUParameter());
    fa.vMin = static_cast<float>(adaptor.FirstVParameter());
    fa.vMax = static_cast<float>(adaptor.LastVParameter());
    fa.isPeriodicU = adaptor.IsUPeriodic();
    fa.isPeriodicV = adaptor.IsVPeriodic();

    // --- Extract surface-specific parameters FIRST (radius, axis, etc.) ---
    // Must be before curvature computation for analytical surfaces
    extractSurfaceParams(surf, occType, fa);

    // --- Curvature: analytical surfaces need ZERO sampling ---
    // Curvature is constant and known for these types
    switch (occType) {
        case GeomAbs_Plane:
            fa.category = SurfaceCategory::Planar;
            fa.minGaussCurvature = 0.0f;
            fa.maxGaussCurvature = 0.0f;
            fa.meanCurvature = 0.0f;
            fa.rmsCurvature = 0.0f;
            break;
        case GeomAbs_Cylinder:
            fa.category = SurfaceCategory::Cylindrical;
            fa.minGaussCurvature = 0.0f;
            fa.maxGaussCurvature = 0.0f;
            fa.meanCurvature = 0.5f / std::max(fa.radius, 0.001f);
            fa.rmsCurvature = 0.0f;
            break;
        case GeomAbs_Cone:
            fa.category = SurfaceCategory::Conical;
            fa.minGaussCurvature = 0.0f;
            fa.maxGaussCurvature = 0.0f;
            fa.meanCurvature = 0.5f / std::max(fa.radius, 0.001f);
            fa.rmsCurvature = 0.0f;
            break;
        case GeomAbs_Sphere:
            fa.category = SurfaceCategory::Spherical;
            fa.maxGaussCurvature = 1.0f / (fa.sphereRadius * fa.sphereRadius + 1e-6f);
            fa.minGaussCurvature = fa.maxGaussCurvature;
            fa.meanCurvature = 1.0f / std::max(fa.sphereRadius, 0.001f);
            fa.rmsCurvature = fa.maxGaussCurvature;
            break;
        case GeomAbs_Torus:
            fa.category = SurfaceCategory::Toroidal;
            fa.meanCurvature = 0.5f * (1.0f/std::max(fa.majorRadius, 0.001f) + 1.0f/std::max(fa.minorRadius, 0.001f));
            fa.maxGaussCurvature = 1.0f / (fa.majorRadius * fa.minorRadius + 1e-6f);
            fa.minGaussCurvature = -fa.maxGaussCurvature;
            fa.rmsCurvature = fa.maxGaussCurvature;
            break;
        default: {
            // Freeform — minimal curvature sampling (3×3 = 9 points)
            float uRange = fa.uMax - fa.uMin;
            float vRange = fa.vMax - fa.vMin;
            float maxGauss = -std::numeric_limits<float>::max();
            float minGauss = std::numeric_limits<float>::max();
            float sumMean = 0.0f, sumSqGauss = 0.0f;
            int valid = 0;
            int samples = 3; // 3×3 is enough to detect planar vs curved

            for (int ui = 0; ui < samples; ++ui) {
                for (int vi = 0; vi < samples; ++vi) {
                    float u = fa.uMin + uRange * (ui + 0.5f) / samples;
                    float v = fa.vMin + vRange * (vi + 0.5f) / samples;
                    u = std::clamp(u, fa.uMin, fa.uMax);
                    v = std::clamp(v, fa.vMin, fa.vMax);
                    try {
                        GeomLProp_SLProps props(surf, u, v, 2, 1e-6);
                        if (props.IsCurvatureDefined()) {
                            float g = static_cast<float>(props.GaussianCurvature());
                            float m = static_cast<float>(props.MeanCurvature());
                            maxGauss = std::max(maxGauss, g);
                            minGauss = std::min(minGauss, g);
                            sumMean += m;
                            sumSqGauss += g * g;
                            ++valid;
                        }
                    } catch (...) {}
                }
            }

            if (valid > 0) {
                fa.meanCurvature = sumMean / valid;
                fa.minGaussCurvature = minGauss;
                fa.maxGaussCurvature = maxGauss;
                fa.rmsCurvature = std::sqrt(sumSqGauss / valid);
            }

            SurfaceCategory occCat = occToCategory(occType);
            fa.category = reclassifyByCurvature(occCat, fa.maxGaussCurvature, fa.meanCurvature);
            break;
        }
    }

    // AABB (fast — OCC uses stored bounding box)
    try {
        Bnd_Box box;
        BRepBndLib::Add(face, box);
        if (!box.IsVoid()) {
            Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
            box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            fa.localAABB.min = Vec3(static_cast<float>(xmin), static_cast<float>(ymin), static_cast<float>(zmin));
            fa.localAABB.max = Vec3(static_cast<float>(xmax), static_cast<float>(ymax), static_cast<float>(zmax));
        }
    } catch (...) {}

    // Fast area estimate from AABB (good enough for downstream decisions)
    Vec3 ext = fa.localAABB.extent();
    fa.area = 2.0f * (ext.x * ext.y + ext.y * ext.z + ext.x * ext.z); // upper-bound AABB surface area

    // Perimeter not needed by downstream; skip BRepGProp::LinearProperties
    fa.perimeter = 2.0f * (ext.x + ext.y + ext.z + ext.x + ext.y + ext.z); // AABB wire perimeter

    // Height for cylindrical/conical
    if (fa.category == SurfaceCategory::Cylindrical || fa.category == SurfaceCategory::Conical) {
        fa.height = fa.isPeriodicV ? (fa.vMax - fa.vMin) : fa.localAABB.diagonal();
    }

    return fa;
}

// ------------------------------------------------------------------
// Build face neighbor graph via shared edges
// ------------------------------------------------------------------
void BRepAnalyzer::Impl::buildNeighborGraph(ShapeAnalysis& sa, const TopoDS_Shape& shape) {
    // Build edge→faces map once (O(E log E))
    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaceMap;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaceMap);

    // Build face index lookup once
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

    // Pre-build face→edges map to avoid per-face MapShapes calls
    std::vector<std::vector<int>> faceEdges(sa.faces.size());
    for (int ek = 1; ek <= edgeToFaceMap.Extent(); ++ek) {
        const TopTools_ListOfShape& facesOnEdge = edgeToFaceMap.FindFromIndex(ek);
        for (auto it = facesOnEdge.begin(); it != facesOnEdge.end(); ++it) {
            int fi = faceMap.FindIndex(TopoDS::Face(*it)) - 1;
            if (fi >= 0) {
                faceEdges[fi].push_back(ek);
            }
        }
    }

    // For each face, find neighbors via its incident edges
    for (int fi = 0; fi < static_cast<int>(sa.faces.size()); ++fi) {
        auto& fa = sa.faces[fi];
        for (int ek : faceEdges[fi]) {
            const TopTools_ListOfShape& facesOnEdge = edgeToFaceMap.FindFromIndex(ek);
            for (auto it = facesOnEdge.begin(); it != facesOnEdge.end(); ++it) {
                int nIdx = faceMap.FindIndex(TopoDS::Face(*it)) - 1;
                if (nIdx >= 0 && nIdx != fi) {
                    fa.neighborFaces.push_back(nIdx);
                }
            }
        }
        // Deduplicate
        std::sort(fa.neighborFaces.begin(), fa.neighborFaces.end());
        fa.neighborFaces.erase(
            std::unique(fa.neighborFaces.begin(), fa.neighborFaces.end()),
            fa.neighborFaces.end());
    }
}

// ------------------------------------------------------------------
// Analyze a single shape
// ------------------------------------------------------------------
ShapeAnalysis BRepAnalyzer::Impl::analyzeOne(const TopoDS_Shape& shape, const std::string& shapeKey) {
    ShapeAnalysis sa;
    sa.shapeKey = shapeKey;

    if (shape.IsNull()) return sa;

    // Step 1: Analyze each face
    int faceIndex = 0;
    TopExp_Explorer faceExp(shape, TopAbs_FACE);
    for (; faceExp.More(); faceExp.Next()) {
        TopoDS_Face face = TopoDS::Face(faceExp.Current());
        if (face.IsNull()) continue;

        FaceAnalysis fa = analyzeFace(face, faceIndex);
        sa.faces.push_back(std::move(fa));
        ++faceIndex;
    }

    // Step 2: Build face neighbor graph (efficient — uses OCC's MapShapesAndAncestors)
    buildNeighborGraph(sa, shape);

    // Step 3: Aggregate stats (boundary detection skipped — ShellExtractor handles visibility)
    for (const auto& fa : sa.faces) {
        switch (fa.category) {
            case SurfaceCategory::Planar:      ++sa.planarCount; break;
            case SurfaceCategory::Cylindrical: ++sa.cylindricalCount; break;
            case SurfaceCategory::Conical:     ++sa.conicalCount; break;
            case SurfaceCategory::Spherical:   ++sa.sphericalCount; break;
            case SurfaceCategory::Toroidal:    ++sa.toroidalCount; break;
            case SurfaceCategory::Extrusion:   ++sa.extrusionCount; break;
            case SurfaceCategory::Revolution:  ++sa.revolutionCount; break;
            case SurfaceCategory::Freeform:    ++sa.freeformCount; break;
            default: break;
        }
        sa.totalSurfaceArea += fa.area;
        sa.bbox.expand(fa.localAABB);
    }

    MF_INFO("BRepAnalyzer: '{}' — {} faces: P={} C={} Co={} S={} T={} E={} R={} F={}",
            sa.shapeKey, sa.faces.size(),
            sa.planarCount, sa.cylindricalCount, sa.conicalCount,
            sa.sphericalCount, sa.toroidalCount,
            sa.extrusionCount, sa.revolutionCount, sa.freeformCount);

    return sa;
}

} // namespace mf
