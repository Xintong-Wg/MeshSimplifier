#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include "Geometry/SurfaceClassifier.h"
#include <vector>
#include <functional>

namespace mf {

// ------------------------------------------------------------------
// Mesh simplification strategies
// ------------------------------------------------------------------
enum class SimplifyMode { Ratio, TargetFileSizeMB };

struct SimplifyParams {
    SimplifyMode mode = SimplifyMode::Ratio;
    float targetRatio = 0.5f;           // Ratio mode: target triangle ratio
    float targetFileSizeMB = 5.0f;      // FileSize mode: target export size in MB
    float targetError = 5e-2f;          // base error threshold (relative to bbox diagonal)
    float aggressive = 0.0f;            // 0..1 extra simplification bias
    bool  preserveBoundary = false;         // CAD parts are closed solids — no real mesh boundary
    bool  useSloppyFallback = true;     // allow meshopt_simplifySloppy when normal simplify can't reach target
    int   maxPasses = 3;                // progressive simplification passes
};

// Visual fidelity guided simplification result
struct SimplifyResult {
    MeshData mesh;
    float achievedRatio = 1.0f;         // actual triangle ratio achieved
    float usedError = 0.0f;             // error threshold that was used
    bool  usedSloppy = false;           // whether sloppy fallback was used
    int   passes = 0;                   // number of simplification passes performed
};

// Estimate export file size (MB) for a mesh
float estimateFileSizeMB(const MeshData& mesh);

// Binary search to find ratio for a target file size
float ratioForTargetFileSize(const MeshData& mesh, float targetMB);

class Simplifier {
public:
    // meshoptimizer-based simplification with visual fidelity
    // Uses multi-pass progressive simplification + sloppy fallback
    static SimplifyResult simplifyMeshOptimizer(const MeshData& input, const SimplifyParams& params);

    // DEPRECATED: Fast Quadric stub — kept for API compat, delegates to meshoptimizer
    static MeshData simplifyQuadric(const MeshData& input, const SimplifyParams& params);

    // Auto-pick strategy — always uses meshoptimizer for quality
    static SimplifyResult simplify(const MeshData& input, const SimplifyParams& params);
};

// ------------------------------------------------------------------
// LOD generator
// ------------------------------------------------------------------
class LODGenerator {
public:
    struct Params {
        uint32_t levels = 4;
        float    reductionFactor = 0.5f; // each level halves triangles
        float    baseScreenSize = 1000.0f;

        // Distance-based LOD
        bool useDistanceLOD = false;
        float lodDistances[4] = {10.0f, 50.0f, 200.0f, 1000.0f};

        // Cluster LOD
        bool useClusterLOD = false;
        float clusterThreshold = 500.0f;  // distance beyond which objects merge
        float clusterGridSize = 100.0f;   // spatial grid cell size
    };

    static std::vector<LODLevel> generate(const MeshData& baseMesh, const Params& params);

    // Enhanced: generate LODs with surface tags for feature-aware reduction
    static std::vector<LODLevel> generateWithTags(
        const MeshData& baseMesh,
        const std::vector<SurfaceTag>& tags,
        const Params& params);

    // Cluster LOD: merge a group of meshes into one simplified mesh
    static SimplifyResult mergeAndSimplify(
        const std::vector<const MeshData*>& meshes,
        const std::vector<Mat4>& transforms,
        float targetRatio);

    // Pick LOD level based on distance (not just screen pixels)
    static int pickDistanceLOD(float distance, const Params& params);
};

// ------------------------------------------------------------------
// Geometry hashing for instancing
// ------------------------------------------------------------------
struct GeometryHash {
    uint64_t vertexHash = 0;
    uint64_t indexHash = 0;
    AABB bbox;

    bool operator==(const GeometryHash& o) const {
        return vertexHash == o.vertexHash && indexHash == o.indexHash;
    }
};

struct GeometryHashHasher {
    size_t operator()(const GeometryHash& h) const {
        return h.vertexHash ^ (h.indexHash << 1);
    }
};

GeometryHash computeGeometryHash(const MeshData& mesh);

// ------------------------------------------------------------------
// Streaming / progressive loading
// ------------------------------------------------------------------
struct StreamLoadRequest {
    std::string shapeKey;
    int priority = 5;  // 1=highest (visible), 10=lowest (far)
    bool isVisible = true;
};

class StreamLoader {
public:
    StreamLoader() = default;
    ~StreamLoader() = default;

    void setMemoryBudget(size_t maxBytes) { m_memoryBudget = maxBytes; }
    void setMaxConcurrent(int n) { m_maxConcurrent = n; }

    void enqueue(const StreamLoadRequest& req);
    void processPending();

    size_t loadedCount() const { return m_loaded; }
    size_t pendingCount() const { return m_requests.size(); }
    size_t memoryUsed() const { return m_memoryUsed; }
    bool isComplete() const { return m_requests.empty(); }

private:
    size_t m_memoryBudget = 512 * 1024 * 1024;
    int m_maxConcurrent = 4;
    size_t m_loaded = 0;
    size_t m_memoryUsed = 0;
    std::vector<StreamLoadRequest> m_requests;
};

} // namespace mf
