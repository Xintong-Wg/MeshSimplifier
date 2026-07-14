#include "Geometry/PrimitiveRecognizer.h"
#include "Core/Logger.h"
#include "Geometry/BRepAnalyzer.h"

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mf {

// ------------------------------------------------------------------
// Procedural mesh generation for each proxy type
// ------------------------------------------------------------------
MeshData ProxyGeometry::generateMesh() const {
    MeshData mesh;

    switch (type) {
        case ProxyType::Cylinder: {
            Vec3 base(params[0], params[1], params[2]);
            Vec3 axis(params[4], params[5], params[6]);
            float r = params[8], h = params[9];
            uint32_t slices = std::max(8u, recommendedSlices);

            // Build orthonormal basis from axis
            Vec3 u, v;
            if (std::abs(axis.x) < 0.9f) { u = Vec3(1, 0, 0); u = glm::normalize(u - axis * glm::dot(u, axis)); }
            else { u = Vec3(0, 1, 0); u = glm::normalize(u - axis * glm::dot(u, axis)); }
            v = glm::normalize(glm::cross(axis, u));

            // Two rings + optional caps
            for (uint32_t si = 0; si <= 1; ++si) {
                Vec3 c = base + axis * (si * h);
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    float angle = 2.0f * 3.1415926535f * ci / slices;
                    Vec3 p = c + r * (u * std::cos(angle) + v * std::sin(angle));
                    Vec3 n = glm::normalize(u * std::cos(angle) + v * std::sin(angle));

                    Vertex vtx;
                    vtx.position = p; vtx.normal = n;
                    vtx.uv = Vec2(static_cast<float>(ci) / slices, static_cast<float>(si));
                    vtx.tangent = Vec4(1, 0, 0, 1);
                    mesh.vertices.push_back(vtx);
                }
            }
            for (uint32_t ci = 0; ci < slices; ++ci) {
                uint32_t a = ci, b = (ci + 1) % slices;
                uint32_t c = slices + ci, d = slices + (ci + 1) % slices;
                mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
                mesh.indices.push_back(b); mesh.indices.push_back(d); mesh.indices.push_back(c);
            }
            break;
        }
        case ProxyType::Sphere: {
            Vec3 center(params[0], params[1], params[2]);
            float r = params[8];
            uint32_t slices = std::max(8u, recommendedSlices);
            uint32_t stacks = std::max(4u, recommendedStacks);

            for (uint32_t si = 0; si <= stacks; ++si) {
                float phi = 3.1415926535f * si / stacks;
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    float theta = 2.0f * 3.1415926535f * ci / slices;
                    Vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
                    Vec3 p = center + r * n;
                    Vertex vtx;
                    vtx.position = p; vtx.normal = n;
                    vtx.uv = Vec2(static_cast<float>(ci) / slices, static_cast<float>(si) / stacks);
                    vtx.tangent = Vec4(1, 0, 0, 1);
                    mesh.vertices.push_back(vtx);
                }
            }
            for (uint32_t si = 0; si < stacks; ++si)
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    uint32_t a = si * slices + ci, b = si * slices + (ci+1) % slices;
                    uint32_t c = (si+1) * slices + ci, d = (si+1) * slices + (ci+1) % slices;
                    mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
                    mesh.indices.push_back(b); mesh.indices.push_back(d); mesh.indices.push_back(c);
                }
            break;
        }
        case ProxyType::Box: {
            Vec3 bmin(params[0], params[1], params[2]);
            Vec3 bmax(params[4], params[5], params[6]);
            Vec3 corners[8] = {
                {bmin.x,bmin.y,bmin.z}, {bmax.x,bmin.y,bmin.z},
                {bmax.x,bmax.y,bmin.z}, {bmin.x,bmax.y,bmin.z},
                {bmin.x,bmin.y,bmax.z}, {bmax.x,bmin.y,bmax.z},
                {bmax.x,bmax.y,bmax.z}, {bmin.x,bmax.y,bmax.z}};
            Vec3 norms[6] = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
            for (int i = 0; i < 8; ++i) {
                Vertex vtx; vtx.position = corners[i]; vtx.normal = Vec3(0,1,0);
                vtx.uv = Vec2(0,0); vtx.tangent = Vec4(1,0,0,1);
                mesh.vertices.push_back(vtx);
            }
            // 6 faces × 2 triangles each = 12 triangles
            int faces[6][4] = {{0,1,2,3},{5,4,7,6},{0,4,5,1},{3,2,6,7},{0,3,7,4},{1,5,6,2}};
            for (int f = 0; f < 6; ++f) {
                mesh.indices.push_back(faces[f][0]); mesh.indices.push_back(faces[f][1]); mesh.indices.push_back(faces[f][2]);
                mesh.indices.push_back(faces[f][0]); mesh.indices.push_back(faces[f][2]); mesh.indices.push_back(faces[f][3]);
            }
            break;
        }
        default: break;
    }

    mesh.computeAABB();
    return mesh;
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class PrimitiveRecognizer::Impl {
public:
    float planeMergeAngle = 1.0f;
    float boxAngleTol = 2.0f;
    float pipeAxisTol = 5.0f;
    float minFaceArea = 5.0f;

    PrimitiveRecognitionResult recognize(
        const std::unordered_map<std::string, ClassificationResult>& classifications,
        const std::unordered_map<std::string, ShapeAnalysis>& analyses);

    void recognizePlanesAndBoxes(const std::unordered_map<std::string, ClassificationResult>& classifications,
                                  const std::unordered_map<std::string, ShapeAnalysis>& analyses,
                                  PrimitiveRecognitionResult& result);
    void recognizeCylindersAndPipes(const std::unordered_map<std::string, ClassificationResult>& classifications,
                                     const std::unordered_map<std::string, ShapeAnalysis>& analyses,
                                     PrimitiveRecognitionResult& result);
    void recognizeOtherPrimitives(const std::unordered_map<std::string, ClassificationResult>& classifications,
                                   const std::unordered_map<std::string, ShapeAnalysis>& analyses,
                                   PrimitiveRecognitionResult& result);
};

PrimitiveRecognizer::PrimitiveRecognizer() : m_impl(std::make_unique<Impl>()) {}
PrimitiveRecognizer::~PrimitiveRecognizer() = default;

PrimitiveRecognitionResult PrimitiveRecognizer::recognize(
    const std::unordered_map<std::string, ClassificationResult>& classifications,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses) {
    return m_impl->recognize(classifications, analyses);
}

// ------------------------------------------------------------------
PrimitiveRecognitionResult PrimitiveRecognizer::Impl::recognize(
    const std::unordered_map<std::string, ClassificationResult>& classifications,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses) {

    PrimitiveRecognitionResult result;

    recognizePlanesAndBoxes(classifications, analyses, result);
    recognizeCylindersAndPipes(classifications, analyses, result);
    recognizeOtherPrimitives(classifications, analyses, result);

    MF_INFO("PrimitiveRecognizer: P={} C={} Co={} S={} T={} Box={} Pipe={} ({} faces replaced)",
            result.planeCount, result.cylinderCount, result.coneCount,
            result.sphereCount, result.torusCount, result.boxCount, result.pipeCount,
            result.totalFacesReplaced);

    return result;
}

// ------------------------------------------------------------------
// Recognize individual planes and box groupings
// ------------------------------------------------------------------
void PrimitiveRecognizer::Impl::recognizePlanesAndBoxes(
    const std::unordered_map<std::string, ClassificationResult>& classifications,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses,
    PrimitiveRecognitionResult& result) {

    // Collect all planar groups across all shapes
    struct PlanarGroupInfo {
        std::string shapeKey;
        const ClassificationResult::PlanarGroup* group = nullptr;
    };
    std::vector<PlanarGroupInfo> allPlanarGroups;

    for (auto& [sk, cr] : classifications) {
        for (auto& pg : cr.planarGroups) {
            if (pg.totalArea >= minFaceArea) {
                allPlanarGroups.push_back({sk, &pg});
            }
        }
    }

    // Box detection: find triples of mutually orthogonal planar groups
    // with opposite parallel pairs
    std::vector<bool> used(allPlanarGroups.size(), false);

    for (size_t i = 0; i < allPlanarGroups.size(); ++i) {
        if (used[i]) continue;
        auto& gi = allPlanarGroups[i];

        // Check for box: find opposite parallel plane, then orthogonal pairs
        std::vector<size_t> candidates = {i};
        Vec3 norm1 = gi.group->normal;

        for (size_t j = i + 1; j < allPlanarGroups.size(); ++j) {
            if (used[j]) continue;
            auto& gj = allPlanarGroups[j];
            float dot = std::abs(glm::dot(norm1, gj.group->normal));

            if (dot > std::cos(boxAngleTol * 3.14159f / 180.0f)) {
                // Same normal = candidate parallel face
                candidates.push_back(j);
            }
        }

        // A box needs 6 planar groups forming 3 orthogonal pairs
        // Simplified: recognize each planar group as an individual Plane primitive
        for (auto ci : candidates) {
            auto& info = allPlanarGroups[ci];
            PrimitiveGroup pg;
            pg.groupId = "plane_" + info.shapeKey + "_" + std::to_string(result.planeCount);
            pg.type = ProxyType::Plane;
            pg.isExact = true;

            ProxyGeometry proxy;
            proxy.type = ProxyType::Plane;
            proxy.params[0] = info.group->faceIndices.empty() ? 0.0f : info.group->normal.x;
            proxy.params[1] = info.group->normal.y;
            proxy.params[2] = info.group->normal.z;
            proxy.params[4] = info.group->normal.x;
            proxy.params[5] = info.group->normal.y;
            proxy.params[6] = info.group->normal.z;
            proxy.params[8] = info.group->offset;
            pg.proxy = proxy;
            pg.shapeKeys.push_back(info.shapeKey);
            pg.faceIndices = info.group->faceIndices;
            pg.maxDeviation = 0.0f;
            pg.meanDeviation = 0.0f;

            result.groups.push_back(std::move(pg));
            ++result.planeCount;
            result.totalFacesReplaced += static_cast<int>(info.group->faceIndices.size());
            used[ci] = true;
        }
    }
}

// ------------------------------------------------------------------
// Recognize cylinders and connected pipes
// ------------------------------------------------------------------
void PrimitiveRecognizer::Impl::recognizeCylindersAndPipes(
    const std::unordered_map<std::string, ClassificationResult>& classifications,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses,
    PrimitiveRecognitionResult& result) {

    for (auto& [sk, cr] : classifications) {
        auto anaIt = analyses.find(sk);
        if (anaIt == analyses.end()) continue;

        for (auto& cf : cr.classifiedFaces) {
            if (cf.category != SurfaceCategory::Cylindrical) continue;

            const FaceAnalysis* fa = cf.analysis;
            if (!fa || fa->area < minFaceArea) continue;

            PrimitiveGroup pg;
            pg.groupId = "cyl_" + sk + "_" + std::to_string(cf.faceIndex);
            pg.type = ProxyType::Cylinder;
            pg.isExact = (fa->occSurfaceType == 1); // GeomAbs_Cylinder = 1

            ProxyGeometry proxy;
            proxy.type = ProxyType::Cylinder;
            // Store axis direction and radius from face analysis
            proxy.params[0] = 0; proxy.params[1] = 0; proxy.params[2] = 0; // base center (will be set from bbox)
            proxy.params[4] = fa->axisDirection.x;
            proxy.params[5] = fa->axisDirection.y;
            proxy.params[6] = fa->axisDirection.z;
            proxy.params[8] = fa->radius;
            proxy.params[9] = fa->height;
            proxy.recommendedSlices = cf.recommendedSlices;
            proxy.recommendedStacks = 2;

            pg.proxy = proxy;
            pg.shapeKeys.push_back(sk);
            pg.faceIndices.push_back(cf.faceIndex);
            pg.classifiedFaces.push_back(&cf);
            pg.maxDeviation = 0.0f;
            pg.meanDeviation = 0.0f;

            result.groups.push_back(std::move(pg));
            ++result.cylinderCount;
            ++result.totalFacesReplaced;
        }
    }

    // Pipe detection: groups of connected cylinders with similar axis
    // (simplified: left for Phase 3)
}

// ------------------------------------------------------------------
// Recognize cones, spheres, toruses
// ------------------------------------------------------------------
void PrimitiveRecognizer::Impl::recognizeOtherPrimitives(
    const std::unordered_map<std::string, ClassificationResult>& classifications,
    const std::unordered_map<std::string, ShapeAnalysis>& analyses,
    PrimitiveRecognitionResult& result) {

    for (auto& [sk, cr] : classifications) {
        auto anaIt = analyses.find(sk);
        if (anaIt == analyses.end()) continue;

        for (auto& cf : cr.classifiedFaces) {
            const FaceAnalysis* fa = cf.analysis;
            if (!fa || fa->area < minFaceArea) continue;

            PrimitiveGroup pg;
            pg.shapeKeys.push_back(sk);
            pg.faceIndices.push_back(cf.faceIndex);
            pg.classifiedFaces.push_back(&cf);

            switch (cf.category) {
                case SurfaceCategory::Conical: {
                    pg.groupId = "cone_" + sk + "_" + std::to_string(cf.faceIndex);
                    pg.type = ProxyType::Cone;
                    pg.isExact = (fa->occSurfaceType == 2); // GeomAbs_Cone

                    ProxyGeometry proxy;
                    proxy.type = ProxyType::Cone;
                    proxy.params[4] = fa->axisDirection.x;
                    proxy.params[5] = fa->axisDirection.y;
                    proxy.params[6] = fa->axisDirection.z;
                    proxy.params[8] = fa->radius;
                    proxy.params[9] = fa->height;
                    proxy.params[10] = fa->coneAngle;
                    proxy.recommendedSlices = cf.recommendedSlices;
                    proxy.recommendedStacks = cf.recommendedStacks;
                    pg.proxy = proxy;
                    ++result.coneCount;
                    break;
                }
                case SurfaceCategory::Spherical: {
                    pg.groupId = "sph_" + sk + "_" + std::to_string(cf.faceIndex);
                    pg.type = ProxyType::Sphere;
                    pg.isExact = (fa->occSurfaceType == 3);

                    ProxyGeometry proxy;
                    proxy.type = ProxyType::Sphere;
                    proxy.params[0] = fa->sphereCenter.x;
                    proxy.params[1] = fa->sphereCenter.y;
                    proxy.params[2] = fa->sphereCenter.z;
                    proxy.params[8] = fa->sphereRadius;
                    proxy.recommendedSlices = cf.recommendedSlices;
                    proxy.recommendedStacks = cf.recommendedStacks / 2;
                    pg.proxy = proxy;
                    ++result.sphereCount;
                    break;
                }
                case SurfaceCategory::Toroidal: {
                    pg.groupId = "tor_" + sk + "_" + std::to_string(cf.faceIndex);
                    pg.type = ProxyType::Torus;
                    pg.isExact = (fa->occSurfaceType == 4);

                    ProxyGeometry proxy;
                    proxy.type = ProxyType::Torus;
                    proxy.params[4] = fa->axisDirection.x;
                    proxy.params[5] = fa->axisDirection.y;
                    proxy.params[6] = fa->axisDirection.z;
                    proxy.params[8] = fa->majorRadius;
                    proxy.params[9] = fa->minorRadius;
                    proxy.recommendedSlices = cf.recommendedSlices;
                    proxy.recommendedStacks = cf.recommendedStacks;
                    pg.proxy = proxy;
                    ++result.torusCount;
                    break;
                }
                default: continue;
            }
            result.groups.push_back(std::move(pg));
            ++result.totalFacesReplaced;
        }
    }
}

} // namespace mf
