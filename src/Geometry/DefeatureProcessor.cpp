#include "Geometry/DefeatureProcessor.h"
#include "Core/Logger.h"

#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_ShapeEnum.hxx>
#include <opencascade/BRepAdaptor_Surface.hxx>
#include <opencascade/Geom_Surface.hxx>
#include <opencascade/Geom_CylindricalSurface.hxx>
#include <opencascade/Geom_ConicalSurface.hxx>
#include <opencascade/GeomAbs_SurfaceType.hxx>
#include <opencascade/gp_Cylinder.hxx>
#include <opencascade/gp_Cone.hxx>
#include <opencascade/gp_Ax1.hxx>
#include <opencascade/gp_Dir.hxx>
#include <opencascade/TopTools_IndexedMapOfShape.hxx>
#include <opencascade/TopExp.hxx>
#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Bnd_Box.hxx>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mf {

// ------------------------------------------------------------------
// Feature type classification
// ------------------------------------------------------------------
enum class FeatureType {
    None,
    SmallHole,       // cylindrical face with small radius, forms a hole
    SmallFillet,     // blend surface with small radius
    SmallChamfer,    // narrow planar strip at 45° between two faces
    SmallProtrusion, // small isolated boss/pad
    ThreadFeature,   // continuous helical/repeating small faces
    InternalFace     // face fully enclosed by other faces (from analysis)
};

struct DetectedFeature {
    FeatureType type;
    std::string shapeKey;
    int faceIndex;
    float characteristicSize = 0.0f; // radius, width, or height
    bool shouldRemove = false;
};

// ------------------------------------------------------------------
// Detect if a cylindrical face is a small hole
// ------------------------------------------------------------------
static bool isSmallHole(const FaceAnalysis& fa, const DefeatureParams& params) {
    if (fa.category != SurfaceCategory::Cylindrical) return false;
    // A hole is a cylindrical face that is closed in the U direction (full 360°)
    // and has radius below threshold
    if (!fa.isPeriodicU) return false;
    return fa.radius < params.minHoleRadius;
}

// ------------------------------------------------------------------
// Detect if a face is a blend/fillet surface
// ------------------------------------------------------------------
static bool isFilletFace(const FaceAnalysis& fa, const DefeatureParams& params) {
    // OCC marks blend surfaces as GeomAbs_BSplineSurface or GeomAbs_BezierSurface
    // with specific geometric properties
    if (fa.occSurfaceType == static_cast<int>(GeomAbs_BSplineSurface) ||
        fa.occSurfaceType == static_cast<int>(GeomAbs_BezierSurface)) {

        // Fillets typically have small area and are narrow strips
        if (fa.area < params.minFeatureArea) return true;

        // Check curvature: fillets have high mean curvature in one direction
        if (fa.rmsCurvature > 0.01f && fa.area < params.minFeatureArea * 3.0f) return true;
    }

    // Note: GeomAbs_BlendSurface is not available in all OCC versions.
    // Fillet detection via BSpline + small area + curvature is sufficient.

    return false;
}

// ------------------------------------------------------------------
// Detect chamfer (narrow planar face at ~45° to adjacent faces)
// ------------------------------------------------------------------
static bool isChamferFace(const FaceAnalysis& fa, const DefeatureParams& params,
                           const std::vector<FaceAnalysis>& allFaces) {
    if (fa.category != SurfaceCategory::Planar) return false;
    if (fa.area > params.minFeatureArea * 2.0f) return false;

    // A chamfer is a narrow planar strip — check aspect ratio
    float extent = fa.localAABB.diagonal();
    float estWidth = fa.area / std::max(extent, 0.001f);

    if (estWidth < params.minChamferWidth && fa.area < params.minFeatureArea * 1.5f) {
        // Verify: check if neighbors form ~90° angle through this face
        for (int ni : fa.neighborFaces) {
            if (ni < 0 || ni >= static_cast<int>(allFaces.size())) continue;
            const auto& nb = allFaces[ni];
            if (nb.category == SurfaceCategory::Planar) {
                float dot = std::abs(glm::dot(fa.planeNormal, nb.planeNormal));
                // Chamfer face is at ~45° to main faces
                if (dot > 0.6f && dot < 0.85f) return true;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------------
// Detect small protrusion (isolated boss/pad)
// ------------------------------------------------------------------
static bool isSmallProtrusion(const FaceAnalysis& fa, const DefeatureParams& params,
                               const std::vector<FaceAnalysis>& allFaces) {
    // A protrusion is a set of faces that form a local elevation
    // Simplified heuristic: small face with few neighbors, all at large angles
    if (fa.area > params.minFeatureArea * 2.0f) return false;
    if (fa.neighborFaces.size() > 3) return false; // true protrusions are simple

    // Check if face is nearly parallel to all neighbors (flat top of boss)
    int parallelCount = 0;
    for (int ni : fa.neighborFaces) {
        if (ni < 0 || ni >= static_cast<int>(allFaces.size())) continue;
        const auto& nb = allFaces[ni];
        if (nb.category == SurfaceCategory::Planar) {
            float dot = std::abs(glm::dot(fa.planeNormal, nb.planeNormal));
            if (dot > 0.95f) ++parallelCount;
        }
    }
    return parallelCount == 0 && !fa.neighborFaces.empty();
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class DefeatureProcessor::Impl {
public:
    DefeatureResult processOne(const TopoDS_Shape& shape, const ShapeAnalysis& analysis,
                                const DefeatureParams& params);
    std::vector<DetectedFeature> detectFeatures(const ShapeAnalysis& analysis,
                                                  const DefeatureParams& params);
};

DefeatureProcessor::DefeatureProcessor() : m_impl(std::make_unique<Impl>()) {}
DefeatureProcessor::~DefeatureProcessor() = default;

DefeatureResult DefeatureProcessor::process(
    const std::unordered_map<std::string, TopoDS_Shape>& shapesByKey,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses,
    const DefeatureParams& params) {

    DefeatureResult globalResult;
    std::mutex resultMutex;

    std::vector<std::pair<const std::string*, const TopoDS_Shape*>> items;
    items.reserve(shapesByKey.size());
    for (auto& [key, shp] : shapesByKey) items.emplace_back(&key, &shp);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, items.size()),
        [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                auto [keyPtr, shpPtr] = items[i];
                auto anaIt = analyses.find(*keyPtr);
                if (anaIt == analyses.end()) continue;

                auto shapeResult = m_impl->processOne(*shpPtr, anaIt->second, params);
                std::lock_guard<std::mutex> lock(resultMutex);
                globalResult.featuresRemoved += shapeResult.featuresRemoved;
                globalResult.areaReduction += shapeResult.areaReduction;
                for (auto& f : shapeResult.removedFaces)
                    globalResult.removedFaces.push_back(std::move(f));
                for (auto& h : shapeResult.filledHoles)
                    globalResult.filledHoles.push_back(std::move(h));
            }
        });

    MF_INFO("DefeatureProcessor: {} features removed, {:.1f}mm² area reduced",
            globalResult.featuresRemoved, globalResult.areaReduction);

    return globalResult;
}

DefeatureResult DefeatureProcessor::processShape(
    const TopoDS_Shape& shape, const ShapeAnalysis& analysis,
    const DefeatureParams& params) {
    return m_impl->processOne(shape, analysis, params);
}

// ------------------------------------------------------------------
std::vector<DetectedFeature> DefeatureProcessor::Impl::detectFeatures(
    const ShapeAnalysis& analysis, const DefeatureParams& params) {

    std::vector<DetectedFeature> features;

    for (size_t fi = 0; fi < analysis.faces.size(); ++fi) {
        const auto& fa = analysis.faces[fi];
        DetectedFeature df;
        df.shapeKey = analysis.shapeKey;
        df.faceIndex = static_cast<int>(fi);

        if (isSmallHole(fa, params)) {
            df.type = FeatureType::SmallHole;
            df.characteristicSize = fa.radius;
            df.shouldRemove = true;
            features.push_back(df);
        } else if (isFilletFace(fa, params)) {
            df.type = FeatureType::SmallFillet;
            df.characteristicSize = fa.rmsCurvature > 0.001f ? 1.0f / fa.rmsCurvature : 1.0f;
            df.shouldRemove = true;
            features.push_back(df);
        } else if (isChamferFace(fa, params, analysis.faces)) {
            df.type = FeatureType::SmallChamfer;
            df.characteristicSize = std::sqrt(fa.area);
            df.shouldRemove = true;
            features.push_back(df);
        } else if (isSmallProtrusion(fa, params, analysis.faces)) {
            df.type = FeatureType::SmallProtrusion;
            df.characteristicSize = std::sqrt(fa.area);
            df.shouldRemove = true;
            features.push_back(df);
        } else if (!fa.isOnOuterBoundary && params.removeInternalFeatures) {
            df.type = FeatureType::InternalFace;
            df.characteristicSize = 0.0f;
            df.shouldRemove = true;
            features.push_back(df);
        }
    }

    return features;
}

// ------------------------------------------------------------------
DefeatureResult DefeatureProcessor::Impl::processOne(
    const TopoDS_Shape& shape, const ShapeAnalysis& analysis,
    const DefeatureParams& params) {

    DefeatureResult result;

    auto features = detectFeatures(analysis, params);
    if (features.empty()) return result;

    result.featuresRemoved = features.size();

    // Calculate area reduction
    for (auto& feat : features) {
        if (feat.faceIndex >= 0 && feat.faceIndex < static_cast<int>(analysis.faces.size())) {
            result.areaReduction += analysis.faces[feat.faceIndex].area;
            std::stringstream ss;
            ss << analysis.shapeKey << ":" << feat.faceIndex;
            result.removedFaces.push_back(ss.str());
        }
    }

    // Note: Actual BRep modification (face removal, gap filling) requires
    // OCC BRepAlgoAPI tools and is deferred to a later phase.
    // For now, we detect and flag features. The flagged faces are marked
    // for removal and will be excluded from tessellation.

    if (!features.empty()) {
        MF_INFO("DefeatureProcessor: '{}' — {} features detected (holes, fillets, etc.), {:.1f}mm² to remove",
                analysis.shapeKey, features.size(), result.areaReduction);
    }

    return result;
}

} // namespace mf
