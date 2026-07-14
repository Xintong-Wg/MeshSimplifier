#include "Geometry/SurfaceClassifier.h"
#include "Core/Logger.h"

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <cmath>
#include <algorithm>
#include <map>

namespace mf {

const char* tessellationStrategyName(TessellationStrategy s) {
    switch (s) {
        case TessellationStrategy::PlanarBoundary:    return "PlanarBoundary";
        case TessellationStrategy::CylindricalStrip:  return "CylindricalStrip";
        case TessellationStrategy::ConicalStrip:      return "ConicalStrip";
        case TessellationStrategy::SphericalGrid:     return "SphericalGrid";
        case TessellationStrategy::ToroidalGrid:      return "ToroidalGrid";
        case TessellationStrategy::ExtrusionStrip:    return "ExtrusionStrip";
        case TessellationStrategy::RevolutionRing:    return "RevolutionRing";
        case TessellationStrategy::FreeformAdaptive:  return "FreeformAdaptive";
        case TessellationStrategy::ProxyOmitted:      return "ProxyOmitted";
        default: return "Unknown";
    }
}

// ------------------------------------------------------------------
// Strategy selection: surface category → tessellation strategy
// ------------------------------------------------------------------
static TessellationStrategy selectStrategy(SurfaceCategory cat, const FaceAnalysis& fa) {
    switch (cat) {
        case SurfaceCategory::Planar:
            return TessellationStrategy::PlanarBoundary;

        case SurfaceCategory::Cylindrical:
            return TessellationStrategy::CylindricalStrip;

        case SurfaceCategory::Conical:
            return TessellationStrategy::ConicalStrip;

        case SurfaceCategory::Spherical:
            return TessellationStrategy::SphericalGrid;

        case SurfaceCategory::Toroidal:
            return TessellationStrategy::ToroidalGrid;

        case SurfaceCategory::Extrusion:
            return TessellationStrategy::ExtrusionStrip;

        case SurfaceCategory::Revolution:
            return TessellationStrategy::RevolutionRing;

        case SurfaceCategory::Freeform:
        case SurfaceCategory::Unknown:
        default:
            return TessellationStrategy::FreeformAdaptive;
    }
}

// ------------------------------------------------------------------
// Compute recommended tessellation parameters for each category
// ------------------------------------------------------------------
static void computeTessellationParams(ClassifiedFace& cf, const FaceAnalysis& fa) {
    switch (cf.tessStrategy) {
        case TessellationStrategy::PlanarBoundary: {
            // Large planes: fewer, longer edges. Small planes: more detail.
            cf.maxEdgeLength = std::max(1.0f, std::sqrt(fa.area) * 0.5f);
            cf.recommendedSlices = 0;  // not applicable
            cf.recommendedStacks = 0;
            break;
        }
        case TessellationStrategy::CylindricalStrip: {
            // Slices proportional to radius and visual importance
            float slices = std::max(8.0f, std::min(64.0f, fa.radius * 0.5f));
            cf.recommendedSlices = static_cast<uint32_t>(slices);
            cf.recommendedStacks = 2;  // minimal axial segments for pure cylinders
            cf.maxEdgeLength = fa.height * 0.5f;
            break;
        }
        case TessellationStrategy::ConicalStrip: {
            float slices = std::max(8.0f, std::min(64.0f, fa.radius * 0.5f));
            cf.recommendedSlices = static_cast<uint32_t>(slices);
            cf.recommendedStacks = std::max(4u, static_cast<uint32_t>(fa.height / 50.0f));
            cf.maxEdgeLength = fa.height * 0.2f;
            break;
        }
        case TessellationStrategy::SphericalGrid: {
            float slices = std::max(8.0f, std::min(32.0f, fa.sphereRadius * 0.3f));
            cf.recommendedSlices = static_cast<uint32_t>(slices);
            cf.recommendedStacks = cf.recommendedSlices / 2;
            cf.maxEdgeLength = fa.sphereRadius * 0.3f;
            break;
        }
        case TessellationStrategy::ToroidalGrid: {
            float slices = std::max(12.0f, std::min(48.0f, fa.majorRadius * 0.4f));
            cf.recommendedSlices = static_cast<uint32_t>(slices);
            cf.recommendedStacks = static_cast<uint32_t>(std::max(6.0f, fa.minorRadius * 0.5f));
            cf.maxEdgeLength = fa.minorRadius * 0.3f;
            break;
        }
        case TessellationStrategy::FreeformAdaptive:
        default: {
            cf.recommendedSlices = 0;
            cf.recommendedStacks = 0;
            cf.maxEdgeLength = 1.0f;  // controlled by OCC deflection
            break;
        }
    }
}

// ------------------------------------------------------------------
// Check if two planar faces are coplanar
// ------------------------------------------------------------------
static bool areCoplanar(const FaceAnalysis& a, const FaceAnalysis& b,
                         float normalTolDeg, float offsetTol) {
    float dot = std::abs(glm::dot(a.planeNormal, b.planeNormal));
    float angleDeg = std::acos(std::min(1.0f, dot)) * 180.0f / 3.1415926535f;
    if (angleDeg > normalTolDeg) return false;

    float offsetDiff = std::abs(a.planeOffset - b.planeOffset);
    return offsetDiff < offsetTol;
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class SurfaceClassifier::Impl {
public:
    float planarNormalTol = 1.0f;     // degrees
    float planarOffsetTol = 0.1f;     // mm
    float coplanarAreaThreshold = 10.0f;

    ClassificationResult classifyOne(const ShapeAnalysis& analysis);
    void buildPlanarGroups(ClassificationResult& cr);
    void buildSurfaceTags(ClassificationResult& cr);
};

SurfaceClassifier::SurfaceClassifier() : m_impl(std::make_unique<Impl>()) {
    m_impl->planarNormalTol = m_planarNormalTol;
    m_impl->planarOffsetTol = m_planarOffsetTol;
    m_impl->coplanarAreaThreshold = m_coplanarAreaThreshold;
}
SurfaceClassifier::~SurfaceClassifier() = default;

ClassificationResult SurfaceClassifier::classify(const ShapeAnalysis& analysis) {
    return m_impl->classifyOne(analysis);
}

std::unordered_map<std::string, ClassificationResult>
SurfaceClassifier::classifyBatch(const std::unordered_map<std::string, ShapeAnalysis>& analyses) {
    std::unordered_map<std::string, ClassificationResult> results;
    std::mutex resultsMutex;

    std::vector<std::pair<const std::string*, const ShapeAnalysis*>> items;
    items.reserve(analyses.size());
    for (const auto& [key, val] : analyses) {
        items.emplace_back(&key, &val);
    }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, items.size()),
        [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                auto [keyPtr, valPtr] = items[i];
                auto cr = m_impl->classifyOne(*valPtr);
                std::lock_guard<std::mutex> lock(resultsMutex);
                results[*keyPtr] = std::move(cr);
            }
        });

    return results;
}

// ------------------------------------------------------------------
void SurfaceClassifier::Impl::buildPlanarGroups(ClassificationResult& cr) {
    // Collect all planar faces
    std::vector<int> planarIndices;
    for (auto& cf : cr.classifiedFaces) {
        if (cf.category == SurfaceCategory::Planar) {
            planarIndices.push_back(cf.faceIndex);
        }
    }

    // Union-find on coplanarity
    std::vector<int> parent(planarIndices.size());
    for (size_t i = 0; i < planarIndices.size(); ++i) parent[i] = static_cast<int>(i);

    auto find = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    auto unite = [&](int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };

    for (size_t i = 0; i < planarIndices.size(); ++i) {
        int fi = planarIndices[i];
        const FaceAnalysis& fa = cr.classifiedFaces[fi].analysis ? *cr.classifiedFaces[fi].analysis
                                                                   : FaceAnalysis{};
        for (size_t j = i + 1; j < planarIndices.size(); ++j) {
            int fj = planarIndices[j];
            const FaceAnalysis& fb = cr.classifiedFaces[fj].analysis ? *cr.classifiedFaces[fj].analysis
                                                                   : FaceAnalysis{};
            if (fa.area < coplanarAreaThreshold || fb.area < coplanarAreaThreshold) continue;
            if (areCoplanar(fa, fb, planarNormalTol, planarOffsetTol)) {
                unite(static_cast<int>(i), static_cast<int>(j));
            }
        }
    }

    // Build groups
    std::map<int, std::vector<int>> groups;
    for (size_t i = 0; i < planarIndices.size(); ++i) {
        groups[find(static_cast<int>(i))].push_back(planarIndices[i]);
    }

    for (auto& [root, faces] : groups) {
        ClassificationResult::PlanarGroup pg;
        pg.faceIndices = faces;
        pg.totalArea = 0.0f;

        // Compute average normal and offset
        Vec3 avgNormal(0, 0, 0);
        float avgOffset = 0.0f;
        for (int fi : faces) {
            const auto* fa = cr.classifiedFaces[fi].analysis;
            if (fa) {
                avgNormal += fa->planeNormal;
                avgOffset += fa->planeOffset;
                pg.totalArea += fa->area;
            }
        }
        if (!faces.empty()) {
            avgNormal = glm::normalize(avgNormal);
            avgOffset /= static_cast<float>(faces.size());
        }
        pg.normal = avgNormal;
        pg.offset = avgOffset;

        int groupId = static_cast<int>(cr.planarGroups.size());
        cr.planarGroups.push_back(std::move(pg));

        // Assign group ID to each face
        for (int fi : faces) {
            cr.classifiedFaces[fi].planarGroupId = groupId;
        }
    }
}

// ------------------------------------------------------------------
void SurfaceClassifier::Impl::buildSurfaceTags(ClassificationResult& cr) {
    for (auto& cf : cr.classifiedFaces) {
        SurfaceTag& tag = cf.tag;
        tag.category = cf.category;
        tag.strategy = cf.tessStrategy;
        tag.isPlanarRegion = (cf.category == SurfaceCategory::Planar);
        tag.isCylindricalRegion = (cf.category == SurfaceCategory::Cylindrical);

        // Weight by surface type for feature simplification
        switch (cf.category) {
            case SurfaceCategory::Planar:
                tag.featureWeight = 5.0f;
                break;
            case SurfaceCategory::Cylindrical:
            case SurfaceCategory::Conical:
                tag.featureWeight = 3.0f;
                break;
            case SurfaceCategory::Spherical:
            case SurfaceCategory::Toroidal:
                tag.featureWeight = 2.0f;
                break;
            default:
                tag.featureWeight = 1.0f;
                break;
        }
    }
}

// ------------------------------------------------------------------
ClassificationResult SurfaceClassifier::Impl::classifyOne(const ShapeAnalysis& analysis) {
    ClassificationResult cr;
    cr.shapeKey = analysis.shapeKey;
    cr.classifiedFaces.resize(analysis.faces.size());

    for (size_t i = 0; i < analysis.faces.size(); ++i) {
        const auto& fa = analysis.faces[i];
        ClassifiedFace& cf = cr.classifiedFaces[i];

        cf.faceIndex = fa.faceIndex;
        cf.analysis = &fa;
        cf.category = fa.category;
        cf.tessStrategy = selectStrategy(fa.category, fa);
        cf.planarGroupId = -1;

        // Compute tessellation parameters
        computeTessellationParams(cf, fa);

        // Count by category
        switch (cf.category) {
            case SurfaceCategory::Planar:      ++cr.planarFaceCount; break;
            case SurfaceCategory::Cylindrical: ++cr.cylinderFaceCount; break;
            case SurfaceCategory::Conical:     ++cr.coneFaceCount; break;
            case SurfaceCategory::Spherical:   ++cr.sphereFaceCount; break;
            case SurfaceCategory::Toroidal:    ++cr.torusFaceCount; break;
            case SurfaceCategory::Extrusion:   ++cr.extrusionFaceCount; break;
            case SurfaceCategory::Revolution:  ++cr.revolutionFaceCount; break;
            case SurfaceCategory::Freeform:    ++cr.freeformFaceCount; break;
            default: break;
        }
    }

    // Build coplanar groups
    buildPlanarGroups(cr);

    // Build surface tags for downstream
    buildSurfaceTags(cr);

    MF_INFO("SurfaceClassifier: '{}' — Strategy: P={} C={} Co={} S={} T={} E={} R={} F={} | {} planar groups",
            cr.shapeKey,
            cr.planarFaceCount, cr.cylinderFaceCount, cr.coneFaceCount,
            cr.sphereFaceCount, cr.torusFaceCount,
            cr.extrusionFaceCount, cr.revolutionFaceCount, cr.freeformFaceCount,
            cr.planarGroups.size());

    return cr;
}

} // namespace mf
