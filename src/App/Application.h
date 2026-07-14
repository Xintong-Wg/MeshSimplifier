#pragma once

#include "Core/Types.h"
#include "Geometry/STEPReader.h"
#include "Geometry/BRepEngine.h"
#include "Geometry/BRepAnalyzer.h"
#include "Geometry/SurfaceClassifier.h"
#include "Geometry/AdaptiveTessellator.h"
#include "Geometry/ShellExtractor.h"
#include "Mesh/MeshData.h"
#include "Mesh/Simplifier.h"
#include "Mesh/FeatureSimplifier.h"
#include "Scene/SceneGraph.h"
#include "Renderer/Renderer.h"
#include "UI/UIManager.h"
#include "IO/glTFExporter.h"
#include "IO/STLExporter.h"
#include <memory>
#include <string>
#include <mutex>

struct SDL_Window;
using SDL_GLContext = void*;

namespace mf {

struct PipelineConfig {
    TessellationParams tessellation;
    AdaptiveTessellationParams adaptiveTessellation;
    SimplifyParams simplification;
    FeatureAwareSimplifyParams featureSimplification;
    ShellExtractionParams shellExtraction;
    LODGenerator::Params lod;
    glTFExportOptions exportOpt;
    std::string cacheDir = "./cache";
    bool enableImportCache = false;   // cold imports are faster without writing BRep/mesh cache
    bool useCADAwarePipeline = false; // CAD-aware analysis + adaptive tessellation
    bool useFastImport = true;        // skip shell extraction, parallel tessellation
    int importThreads = 0;            // 0 = auto (use all cores)
};

// Per-part simplification state
struct PartSimplifyState {
    MeshData originalMesh;    // original tessellation result
    MeshData simplifiedMesh;  // simplified result (empty = not simplified)
    bool isSimplified = false;

    // Undo history: stack of previous simplified states (newest last)
    std::vector<MeshData> history;
};

class Application {
public:
    Application();
    ~Application();

    bool init(uint32_t width = 1920, uint32_t height = 1080, const char* title = "MeshForge");
    void shutdown();
    bool isRunning() const;
    void runFrame();
    void processEvents();

    void loadCAD(const std::string& filepath);
    void processAll();
    void processCADAware();  // new CAD-aware pipeline
    void simplifySelected(const std::vector<std::shared_ptr<SceneNode>>& nodes,
                          const SimplifyParams& params);
    void applyBoundingProxy(const std::vector<std::shared_ptr<SceneNode>>& nodes,
                             float margin, int geomType);
    void undoSpecific(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    void resetToOriginal(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    void batchExportSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    void deleteSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    void groupSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    void rebuildViewportMesh();
    void exportglTF(const std::string& filepath);
    void exportSTL(const std::string& filepath);

    std::shared_ptr<Scene> scene() { return m_scene; }

    bool isProcessing() const;
    float progress() const;

private:
    struct PendingSimplifyResult {
        std::string shapeKey;
        MeshData mesh;
    };

    void buildSceneFromCAD();
    void applyPendingSimplifyResults();

    PipelineConfig m_config;

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    Camera m_camera;

    STEPReader m_stepReader;
    std::shared_ptr<STEPResult> m_stepResult;
    std::shared_ptr<Scene> m_scene;
    Renderer m_renderer;
    UIManager m_ui;
    std::shared_ptr<MeshCache> m_meshCache;
    MeshData m_viewportMesh;

    // CAD-aware pipeline state
    BRepAnalyzer m_brepAnalyzer;
    SurfaceClassifier m_surfaceClassifier;
    AdaptiveTessellator m_adaptiveTessellator;
    FeatureSimplifier m_featureSimplifier;
    ShellExtractor m_shellExtractor;
    std::unordered_map<std::string, ShapeAnalysis> m_shapeAnalyses;
    std::unordered_map<std::string, ClassificationResult> m_classifications;
    std::unordered_map<std::string, TaggedMesh> m_taggedMeshes;
    ShellExtractionResult m_shellResult;

    // Tessellated meshes for each unique shapeKey (shared by instances)
    std::unordered_map<std::string, std::shared_ptr<LODMesh>> m_partMeshes;

    // Per-part simplification state (shapeKey -> state)
    std::unordered_map<std::string, PartSimplifyState> m_partSimplifyStates;

    // Each part's index range in the merged viewport mesh (partId -> {indexOffset, indexCount})
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> m_partIndexRanges;

    // Maps AssemblyNode partId -> SceneNode EntityId for viewport selection
    std::unordered_map<std::string, EntityId> m_partNodeIds;

    bool m_processing = false;
    float m_progress = 0.0f;

    // Thread-safe viewport mesh rebuild: worker sets flag, main thread does the rebuild
    bool m_viewportMeshDirty = false;
    std::mutex m_pendingSimplifyMutex;
    std::vector<PendingSimplifyResult> m_pendingSimplifyResults;

    // Track previous selection for highlight updates
    std::unordered_set<EntityId> m_prevSelection;
};

} // namespace mf
