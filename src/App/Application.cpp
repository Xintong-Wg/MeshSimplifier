#include "App/Application.h"
#include "Core/Logger.h"
#include "Core/TaskSystem.h"
#include "Mesh/Simplifier.h"

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <SDL2/SDL.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <chrono>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <algorithm>

namespace mf {

Application::Application() = default;
Application::~Application() = default;

bool Application::init(uint32_t width, uint32_t height, const char* title) {
    MF_INFO("MeshForge v0.1.0 initializing...");

    // Init SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        MF_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    // Compute window size from display bounds (90% width, 88% height, centered)
    SDL_Rect display;
    int winW = static_cast<int>(width);
    int winH = static_cast<int>(height);
    int winX = SDL_WINDOWPOS_CENTERED;
    int winY = SDL_WINDOWPOS_CENTERED;
    if (SDL_GetDisplayBounds(0, &display) == 0) {
        winW = static_cast<int>(display.w * 0.90f);
        winH = static_cast<int>(display.h * 0.88f);
        winX = display.x + (display.w - winW) / 2;
        winY = display.y + (display.h - winH) / 2;
    }
    m_width = static_cast<uint32_t>(winW);
    m_height = static_cast<uint32_t>(winH);

    // OpenGL attributes
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    m_window = SDL_CreateWindow(title,
                                 winX, winY,
                                 winW, winH,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!m_window) {
        MF_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        MF_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(1); // vsync

    m_meshCache = std::make_shared<MeshCache>(m_config.cacheDir);

    if (!m_ui.init(m_window, m_glContext)) {
        MF_ERROR("UI init failed");
        return false;
    }

    m_scene = std::make_shared<Scene>();
    m_ui.setScene(m_scene);

    // Viewport -> SceneTree selection callback
    m_ui.setViewportSelectionCallback([this](const std::vector<EntityId>& ids) {
        m_ui.selectNodesById(ids);
    });

    // Right-click context menu callbacks
    ActionCallbacks actCb;
    actCb.onDeleteSelected = [this]() { deleteSelected(m_ui.selectedNodes()); };
    actCb.onExportSelectedglTF = [this]() {
        auto sel = m_ui.selectedNodes();
        if (sel.empty()) return;
        std::string path = m_ui.saveFileDialog(false);
        if (!path.empty()) {
            glTFExporter exporter;
            std::vector<SceneNode*> nodes;
            for (auto& n : sel) nodes.push_back(n.get());
            exporter.exportNodes(nodes, path, m_config.exportOpt);
        }
    };
    actCb.onExportSelectedSTL = [this]() {
        auto sel = m_ui.selectedNodes();
        if (sel.empty()) return;
        std::string path = m_ui.saveFileDialog(true);
        if (!path.empty()) {
            STLExporter exporter;
            std::vector<SceneNode*> nodes;
            for (auto& n : sel) nodes.push_back(n.get());
            exporter.exportNodes(nodes, path);
        }
    };
    actCb.onGroupSelected = [this]() { groupSelected(m_ui.selectedNodes()); };
    actCb.onFocusOnSelection = [this]() {
        auto sel = m_ui.selectedNodes();
        if (!sel.empty()) {
            const auto aabb = sel[0]->worldAABB();
            if (!aabb.isEmpty()) {
                rebuildViewportMesh();
                m_ui.setMeshForViewport(&m_viewportMesh);
            }
        }
    };
    m_ui.setActionCallbacks(actCb);

    m_camera.aspect = static_cast<float>(width) / static_cast<float>(height);

    m_running = true;
    m_lastFrameTime = static_cast<float>(SDL_GetTicks()) / 1000.0f;

    MF_INFO("Application initialized (SDL2 + OpenGL)");
    return true;
}

void Application::shutdown() {
    m_ui.shutdown();

    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
    MF_INFO("Application shutdown");
}

bool Application::isRunning() const {
    return m_running;
}

void Application::processEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            m_running = false;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(m_window)) {
            m_running = false;
        }
    }
}

void Application::runFrame() {
    processEvents();

    float now = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    float dt = now - m_lastFrameTime;
    m_lastFrameTime = now;

    // Handle window resize
    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    if (w > 0 && h > 0) { m_width = uint32_t(w); m_height = uint32_t(h); }

    // Clear
    glViewport(0, 0, w, h);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // UI — begin frame
    m_ui.beginFrame();
    MenuAction action = m_ui.showMainMenu();

    applyPendingSimplifyResults();

    // Rebuild viewport mesh BEFORE endFrame so the viewport sees the new mesh this frame
    if (m_viewportMeshDirty) {
        m_viewportMeshDirty = false;
        rebuildViewportMesh();
        m_ui.setMeshForViewport(&m_viewportMesh);
    }

    m_ui.endFrame();

    // Handle menu actions
    switch (action) {
    case MenuAction::OpenCAD: {
        std::string path = m_ui.openFileDialog();
        if (!path.empty()) loadCAD(path);
        break;
    }
    case MenuAction::ExportglTF: {
        if (m_scene) {
            std::string path = m_ui.saveFileDialog(false);
            if (!path.empty()) exportglTF(path);
        }
        break;
    }
    case MenuAction::ExportSelectedglTF: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            std::string path = m_ui.saveFileDialog(false);
            if (!path.empty()) {
                glTFExporter exporter;
                std::vector<SceneNode*> nodes;
                for (auto& n : selected) nodes.push_back(n.get());
                if (exporter.exportNodes(nodes, path, m_config.exportOpt)) {
                    MF_INFO("Exported {} selected parts to {}", selected.size(), path);
                }
            }
        }
        break;
    }
    case MenuAction::ExportSelectedSTL: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            std::string path = m_ui.saveFileDialog(true);
            if (!path.empty()) {
                STLExporter exporter;
                std::vector<SceneNode*> nodes;
                for (auto& n : selected) nodes.push_back(n.get());
                if (exporter.exportNodes(nodes, path)) {
                    MF_INFO("Exported {} selected parts to STL {}", selected.size(), path);
                }
            }
        }
        break;
    }
    case MenuAction::BatchExportSelected: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            batchExportSelected(selected);
        }
        break;
    }
    case MenuAction::UndoSelected: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            undoSpecific(selected);
        }
        break;
    }
    case MenuAction::ResetSelected: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            resetToOriginal(selected);
        }
        break;
    }
    case MenuAction::DeleteSelected: {
        auto selected = m_ui.selectedNodes();
        if (!selected.empty()) {
            deleteSelected(selected);
        }
        break;
    }
    case MenuAction::ExportSTL: {
        if (m_scene) {
            std::string path = m_ui.saveFileDialog(true);
            if (!path.empty()) exportSTL(path);
        }
        break;
    }
    case MenuAction::Exit:
        m_running = false;
        break;
    case MenuAction::TessellateAll:
    case MenuAction::GenerateLOD:
    case MenuAction::SimplifyAll:
        processAll();
        break;
    default:
        break;
    }

    // Update simplify settings with current selection
    if (auto* ss = m_ui.simplifySettings()) {
        ss->setTargetNodes(m_ui.selectedNodes());
        if (ss->applyRequested()) {
            ss->resetApplyFlag();
            simplifySelected(m_ui.selectedNodes(), ss->params());
        }
    }

    // Proxy Simplify panel
    if (auto* ps = m_ui.proxySimplify()) {
        ps->setTargetNodes(m_ui.selectedNodes());
        if (ps->applyRequested()) {
            ps->resetApplyFlag();
            applyBoundingProxy(m_ui.selectedNodes(), ps->margin(), ps->geomType());
        }
    }

    // Sync viewport highlight with scene tree selection.
    // Always clear face ranges when in Part mode — they're only for MeshFace mode.
    auto selectedNodes = m_ui.selectedNodes();
    bool isMeshFaceMode = m_ui.viewportSelectionMode() == ViewportPanel::SelectionMode::MeshFace;

    if (!isMeshFaceMode) {
        // Part mode: compute highlight from selected scene tree nodes
        std::unordered_set<EntityId> currSelection;
        for (auto& n : selectedNodes) currSelection.insert(n->id());
        if (currSelection != m_prevSelection) {
            m_prevSelection = currSelection;
            uint32_t hOffset = 0, hCount = 0;
            bool first = true;
            for (auto& node : selectedNodes) {
                if (!node->mesh()) continue;
                std::string key = node->mesh()->name;
                for (auto& [pid, part] : m_stepResult->partsById) {
                    if (part->shapeKey == key) {
                        auto it = m_partIndexRanges.find(pid);
                        if (it != m_partIndexRanges.end()) {
                            if (first) {
                                hOffset = it->second.first;
                                hCount = it->second.second;
                                first = false;
                            } else {
                                uint32_t end = std::max(hOffset + hCount, it->second.first + it->second.second);
                                hOffset = std::min(hOffset, it->second.first);
                                hCount = end - hOffset;
                            }
                        }
                        break;
                    }
                }
            }
            if (hCount > 0) {
                m_ui.clearViewportFaceRanges();
                m_ui.setHighlightForViewport(hOffset, hCount);
            } else {
                m_ui.clearViewportHighlight();
            }
        }
    } else {
        // MeshFace mode: highlight comes from viewport's face ranges
        m_prevSelection.clear();
    }

    // Stats — show viewport mesh triangles and CAD-aware pipeline info
    m_ui.setStats(m_viewportMesh.triangleCount(), 0,
                   dt > 0.001f ? 1.0f / dt : 0.0f);
    m_ui.setPipelineStats(
        static_cast<int>(m_shapeAnalyses.size()),
        static_cast<int>(m_classifications.size()),
        static_cast<int>(m_taggedMeshes.size()),
        static_cast<int>(m_shellResult.totalTrianglesBefore),
        static_cast<int>(m_shellResult.totalTrianglesAfter),
        m_config.useCADAwarePipeline);

    SDL_GL_SwapWindow(m_window);
}

void Application::loadCAD(const std::string& filepath) {
    MF_INFO("Loading CAD file: {}", filepath);
    auto t0 = std::chrono::steady_clock::now();

    // Compute cache dir from file hash for optional BRep binary cache.
    std::hash<std::string> hasher;
    std::string brepCacheDir;
    if (m_config.enableImportCache) {
        brepCacheDir = m_config.cacheDir + "/brep_" + std::to_string(hasher(filepath));
    }

    m_stepResult = m_stepReader.read(filepath, brepCacheDir);
    if (!m_stepResult || !m_stepResult->root) {
        MF_ERROR("Import failed: could not parse CAD file");
        return;
    }

    auto t1 = std::chrono::steady_clock::now();
    MF_INFO("CAD parse: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    m_partMeshes.clear();
    m_partSimplifyStates.clear();
    m_taggedMeshes.clear();

    // Collect shapes into a contiguous vector for parallel processing
    std::vector<std::pair<std::string, TopoDS_Shape>> shapes;
    shapes.reserve(m_stepResult->shapesByKey.size());
    for (auto& [key, shp] : m_stepResult->shapesByKey) {
        shapes.emplace_back(key, shp);
    }

    if (m_config.useCADAwarePipeline) {
        // --- CAD-aware: sequential analysis + parallel tessellation ---
        MF_INFO("Using CAD-aware pipeline with {} shapes...", shapes.size());

        m_shapeAnalyses = m_brepAnalyzer.analyzeBatch(m_stepResult->shapesByKey);
        m_classifications = m_surfaceClassifier.classifyBatch(m_shapeAnalyses);

        auto t2 = std::chrono::steady_clock::now();
        MF_INFO("Analysis+Classification: {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());

        // Pre-allocate storage for each shape (needed for thread safety)
        std::vector<TaggedMesh> taggedResults(shapes.size());
        std::vector<bool> shapeValid(shapes.size(), false);
        m_partSimplifyStates.reserve(shapes.size());

        // Parallel tessellation with TBB
        tbb::parallel_for(tbb::blocked_range<size_t>(0, shapes.size()),
            [&](const tbb::blocked_range<size_t>& r) {
                AdaptiveTessellator localTess;
                AdaptiveTessellationParams localParams = m_config.adaptiveTessellation;
                localParams.parallelMeshing = false;
                for (size_t i = r.begin(); i != r.end(); ++i) {
                    auto& [key, shape] = shapes[i];
                    auto classIt = m_classifications.find(key);
                    if (classIt == m_classifications.end()) continue;

                    TaggedMesh tm = localTess.tessellate(
                        shape, classIt->second, localParams);
                    if (!tm.mesh.vertices.empty()) {
                        tm.mesh.computeAABB();
                        taggedResults[i] = std::move(tm);
                        shapeValid[i] = true;
                    }
                }
            });

        // Single-threaded merge of results
        for (size_t i = 0; i < shapes.size(); ++i) {
            if (!shapeValid[i]) continue;
            auto& [key, shape] = shapes[i];

            m_taggedMeshes[key] = taggedResults[i];

            auto lodMesh = std::make_shared<LODMesh>();
            lodMesh->name = key;
            LODLevel level;
            level.level = 0;
            level.screenSize = 0.0f;
            level.mesh = taggedResults[i].mesh;
            level.mesh.computeAABB();

            PartSimplifyState state;
            state.originalMesh = level.mesh;
            state.isSimplified = false;
            m_partSimplifyStates[key] = std::move(state);

            lodMesh->lods.push_back(std::move(level));
            m_partMeshes[key] = lodMesh;
        }

        auto t3 = std::chrono::steady_clock::now();
        MF_INFO("Tessellation: {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count());

        // Shell extraction (only if not fast import)
        if (!m_config.useFastImport) {
            std::vector<ShellShapeInput> shellInputs;
            for (auto& [id, part] : m_stepResult->partsById) {
                auto it = m_partMeshes.find(part->shapeKey);
                if (it == m_partMeshes.end()) continue;
                Mat4 world(1.0f);
                auto p = part;
                while (p && p->type != AssemblyNode::Type::Root) {
                    world = p->localTransform * world;
                    p = p->parent;
                }
                ShellShapeInput input;
                input.shapeKey = id;
                input.worldTransform = world;
                const auto& lm = it->second->lods[0].mesh;
                input.mesh.vertices.reserve(lm.vertices.size());
                for (const auto& v : lm.vertices) {
                    Vertex wv = v;
                    wv.position = Vec3(world * Vec4(v.position, 1.0f));
                    wv.normal = glm::normalize(Vec3(world * Vec4(v.normal, 0.0f)));
                    input.mesh.vertices.push_back(wv);
                }
                input.mesh.indices = lm.indices;
                shellInputs.push_back(std::move(input));
            }
            m_shellResult = m_shellExtractor.extract(shellInputs, m_config.shellExtraction);
            auto t4 = std::chrono::steady_clock::now();
            MF_INFO("Shell extraction: {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count());
        }
    } else {
        // --- Fast uniform tessellation (parallel, with disk cache) ---
        MF_INFO("Fast uniform tessellation for {} shapes...", shapes.size());

        // Compute cache directory from file hash when cache is enabled.
        std::hash<std::string> hasher;
        std::string cacheDir;
        if (m_config.enableImportCache) {
            cacheDir = m_config.cacheDir + "/" + std::to_string(hasher(filepath));
            std::filesystem::create_directories(cacheDir);
        }

        std::vector<MeshData> meshResults(shapes.size());
        std::vector<bool> shapeValid(shapes.size(), false);
        m_partSimplifyStates.reserve(shapes.size());

        tbb::parallel_for(tbb::blocked_range<size_t>(0, shapes.size()),
            [&](const tbb::blocked_range<size_t>& r) {
                BRepEngine localEngine;
                TessellationParams localParams = m_config.tessellation;
                localParams.parallelMeshing = false;
                for (size_t i = r.begin(); i != r.end(); ++i) {
                    auto& [key, shape] = shapes[i];
                    std::string cachePath;
                    if (m_config.enableImportCache) {
                        cachePath = cacheDir + "/" + key + ".mesh";
                    }

                    // Try cache first
                    MeshData md;
                    if (m_config.enableImportCache && md.loadFromFile(cachePath)) {
                        meshResults[i] = std::move(md);
                        shapeValid[i] = true;
                        continue;
                    }

                    // Tessellate + save to cache
                    try {
                        BRepMesh brepMesh = localEngine.tessellate(shape, localParams);
                        if (!brepMesh.vertices.empty()) {
                            md.vertices = std::move(brepMesh.vertices);
                            md.indices = std::move(brepMesh.indices);
                            md.computeAABB();
                            if (m_config.enableImportCache) {
                                md.saveToFile(cachePath);
                            }
                            meshResults[i] = std::move(md);
                            shapeValid[i] = true;
                        }
                    } catch (...) {
                        MF_WARN("Tessellation failed for shape: {}", key);
                    }
                }
            });

        // Merge results (single-threaded, fast)
        for (size_t i = 0; i < shapes.size(); ++i) {
            if (!shapeValid[i]) continue;
            auto& [key, shape] = shapes[i];

            auto lodMesh = std::make_shared<LODMesh>();
            lodMesh->name = key;
            LODLevel level;
            level.level = 0;
            level.screenSize = 0.0f;
            level.mesh = meshResults[i];
            level.mesh.computeAABB();

            PartSimplifyState state;
            state.originalMesh = level.mesh;
            state.isSimplified = false;
            m_partSimplifyStates[key] = std::move(state);

            lodMesh->lods.push_back(std::move(level));
            m_partMeshes[key] = lodMesh;
        }

        auto t2 = std::chrono::steady_clock::now();
        MF_INFO("Tessellation: {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
    }

    // Build scene tree and viewport mesh ONCE after tessellation is complete
    buildSceneFromCAD();
    m_ui.setScene(m_scene);
    rebuildViewportMesh();

    auto tEnd = std::chrono::steady_clock::now();
    MF_INFO("Total import: {} ms, {} parts, {} verts, {} tris",
            std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count(),
            m_stepResult->partsById.size(),
            m_viewportMesh.vertexCount(), m_viewportMesh.triangleCount());

    size_t pos = filepath.find_last_of("/\\");
    std::string filename = (pos != std::string::npos) ? filepath.substr(pos + 1) : filepath;
    SDL_SetWindowTitle(m_window, ("MeshForge - " + filename).c_str());
    MF_INFO("Import successful: {} parts", m_stepResult->partsById.size());
}

void Application::buildSceneFromCAD() {
    if (!m_stepResult || !m_stepResult->root) return;
    m_scene->clear();
    m_partNodeIds.clear();

    // Two-pass build:
    //   Pass 1: create all scene nodes and map assembly node id -> scene node.
    //   Pass 2: link instance nodes to their prototype scene nodes.
    std::unordered_map<std::string, std::shared_ptr<SceneNode>> nodeMap;

    std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<AssemblyNode>&, SceneNode*)> buildFirstPass =
        [&](const auto& anode, SceneNode* parent) -> std::shared_ptr<SceneNode> {
            auto type = (anode->type == AssemblyNode::Type::Assembly)
                        ? SceneNode::Type::Group : SceneNode::Type::Mesh;
            auto snode = std::make_shared<SceneNode>(anode->name, type);
            snode->setLocalMatrix(anode->localTransform);

            // Attach mesh for part nodes and record partId -> sceneNodeId mapping
            if (type == SceneNode::Type::Mesh) {
                auto it = m_partMeshes.find(anode->shapeKey);
                if (it != m_partMeshes.end()) {
                    snode->setMesh(it->second);
                }
                // Store mapping for viewport pick (use AssemblyNode::id as key)
                m_partNodeIds[anode->id] = snode->id();
            }

            nodeMap[anode->id] = snode;

            if (parent) {
                parent->addChild(snode);
            }

            for (auto& child : anode->children) {
                buildFirstPass(child, snode.get());
            }
            return snode;
        };

    auto rootNode = buildFirstPass(m_stepResult->root, m_scene->root().get());

    // Pass 2: resolve instance prototypes using the map built in pass 1.
    for (auto& [aid, snode] : nodeMap) {
        auto ait = m_stepResult->partsById.find(aid);
        if (ait != m_stepResult->partsById.end() && ait->second->isInstance) {
            auto pit = nodeMap.find(ait->second->prototypeId);
            if (pit != nodeMap.end()) {
                snode->setPrototype(pit->second);
            } else {
                MF_WARN("Instance '{}' references unknown prototype '{}'", aid, ait->second->prototypeId);
            }
        }
    }

    m_scene->detectInstances();
    MF_INFO("Scene built: {} nodes, {} part->node mappings",
            nodeMap.size(), m_partNodeIds.size());
}

void Application::applyPendingSimplifyResults() {
    std::vector<PendingSimplifyResult> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingSimplifyMutex);
        if (m_pendingSimplifyResults.empty()) return;
        pending.swap(m_pendingSimplifyResults);
    }

    size_t applied = 0;
    for (auto& result : pending) {
        auto stateIt = m_partSimplifyStates.find(result.shapeKey);
        if (stateIt != m_partSimplifyStates.end()) {
            if (stateIt->second.isSimplified && !stateIt->second.simplifiedMesh.vertices.empty()) {
                stateIt->second.history.push_back(std::move(stateIt->second.simplifiedMesh));
            }
            stateIt->second.simplifiedMesh = std::move(result.mesh);
            stateIt->second.isSimplified = true;

            auto meshIt = m_partMeshes.find(result.shapeKey);
            if (meshIt != m_partMeshes.end() && !meshIt->second->lods.empty()) {
                meshIt->second->lods[0].mesh = stateIt->second.simplifiedMesh;
            }
            ++applied;
        } else {
            auto meshIt = m_partMeshes.find(result.shapeKey);
            if (meshIt != m_partMeshes.end() && !meshIt->second->lods.empty()) {
                meshIt->second->lods[0].mesh = std::move(result.mesh);
                ++applied;
            }
        }
    }

    if (applied > 0) {
        m_viewportMeshDirty = true;
        MF_INFO("Applied {} simplified mesh results", applied);
    }
}

void Application::processAll() {
    if (m_config.useCADAwarePipeline) {
        processCADAware();
        return;
    }

    if (m_viewportMesh.triangleCount() == 0) {
        MF_WARN("No mesh to process - load a CAD file first");
        return;
    }

    auto task = std::make_shared<Task>("Simplify All", [this](Task* t) {
        t->setProgress(0.05f); t->setMessage("Preparing simplification...");

        SimplifyParams sp;
        sp.targetRatio = 0.25f;
        sp.targetError = 5e-2f;  // more permissive for batch simplification
        sp.preserveBoundary = false;
        sp.maxPasses = 2;
        sp.useSloppyFallback = true;

        size_t total = m_partSimplifyStates.size();
        size_t idx = 0;
        std::vector<std::pair<std::string, MeshData>> inputs;
        inputs.reserve(m_partSimplifyStates.size());
        for (const auto& [key, state] : m_partSimplifyStates) {
            inputs.emplace_back(key, state.originalMesh);
        }

        std::vector<PendingSimplifyResult> results;
        results.reserve(inputs.size());
        for (const auto& [key, originalMesh] : inputs) {
            t->setProgress(0.05f + 0.7f * static_cast<float>(idx) / static_cast<float>(total));
            t->setMessage("Simplifying: " + key);

            auto sr = Simplifier::simplify(originalMesh, sp);
            sr.mesh.computeNormals();
            sr.mesh.computeTangents();
            PendingSimplifyResult pending;
            pending.shapeKey = key;
            pending.mesh = std::move(sr.mesh);
            results.push_back(std::move(pending));
            ++idx;
        }

        t->setProgress(0.85f);
        {
            std::lock_guard<std::mutex> lock(m_pendingSimplifyMutex);
            for (auto& result : results) {
                m_pendingSimplifyResults.push_back(std::move(result));
            }
        }

        t->setProgress(1.0f);
        MF_INFO("Simplification complete: {} parts processed", total);
    });
    TaskSystem::instance().submit(task);
}

void Application::processCADAware() {
    if (m_viewportMesh.triangleCount() == 0) {
        MF_WARN("No mesh to process - load a CAD file first");
        return;
    }

    auto task = std::make_shared<Task>("CAD-Aware Pipeline", [this](Task* t) {
        t->setProgress(0.05f); t->setMessage("Running feature-preserving simplification...");

        size_t total = m_partSimplifyStates.size();
        size_t idx = 0;
        struct CADAwareJobInput {
            std::string shapeKey;
            MeshData source;
            std::vector<SurfaceTag> tags;
        };
        std::vector<CADAwareJobInput> inputs;
        inputs.reserve(m_partSimplifyStates.size());
        for (const auto& [key, state] : m_partSimplifyStates) {
            CADAwareJobInput input;
            input.shapeKey = key;
            input.source = state.originalMesh;
            auto tagsIt = m_taggedMeshes.find(key);
            if (tagsIt != m_taggedMeshes.end()) {
                input.tags = tagsIt->second.triangleTags;
            }
            inputs.push_back(std::move(input));
        }

        std::vector<PendingSimplifyResult> results;
        results.reserve(inputs.size());
        for (const auto& input : inputs) {
            t->setProgress(0.05f + 0.6f * static_cast<float>(idx) / static_cast<float>(total));
            t->setMessage("Feature simplifying: " + input.shapeKey);

            SimplifyParams sp;
            sp.targetRatio = m_config.featureSimplification.targetRatio;
            sp.targetError = m_config.featureSimplification.maxError;
            sp.preserveBoundary = m_config.featureSimplification.preserveBoundary;
            sp.maxPasses = 2;
            sp.useSloppyFallback = true;

            SimplifyResult sr;
            if (!input.tags.empty()) {
                sr = m_featureSimplifier.simplifyV2(input.source, sp, &input.tags);
            } else {
                sr = m_featureSimplifier.simplifyV2(input.source, sp, nullptr);
            }

            sr.mesh.computeNormals();
            sr.mesh.computeTangents();
            PendingSimplifyResult pending;
            pending.shapeKey = input.shapeKey;
            pending.mesh = std::move(sr.mesh);
            results.push_back(std::move(pending));
            ++idx;
        }

        t->setProgress(0.75f);
        {
            std::lock_guard<std::mutex> lock(m_pendingSimplifyMutex);
            for (auto& result : results) {
                m_pendingSimplifyResults.push_back(std::move(result));
            }
        }

        t->setProgress(0.90f); t->setMessage("Generating stats...");

        // Log pipeline statistics
        size_t totalBefore = 0, totalAfter = 0;
        for (const auto& input : inputs) {
            totalBefore += input.source.triangleCount();
        }
        for (const auto& result : results) {
            totalAfter += result.mesh.triangleCount();
        }
        MF_INFO("CAD-Aware simplification: {} → {} triangles ({:.1f}% reduction)",
                totalBefore, totalAfter,
                totalBefore > 0 ? (1.0f - static_cast<float>(totalAfter)/static_cast<float>(totalBefore)) * 100.0f : 0.0f);

        t->setProgress(1.0f);
    });
    TaskSystem::instance().submit(task);
}

void Application::simplifySelected(const std::vector<std::shared_ptr<SceneNode>>& nodes,
                                    const SimplifyParams& params) {
    if (nodes.empty()) {
        MF_WARN("No nodes selected for simplification");
        return;
    }

    MF_INFO("simplifySelected: mode={}, ratio={:.3f}, fileSize={:.2f}MB, error={:.4f}, "
            "boundary={}, sloppy={}, nodes={}",
            (params.mode == SimplifyMode::Ratio) ? "Ratio" : "FileSize",
            params.targetRatio, params.targetFileSizeMB,
            params.targetError, params.preserveBoundary,
            params.useSloppyFallback, nodes.size());

    struct SimplifyJobInput {
        std::string shapeKey;
        std::string nodeName;
        MeshData source;
        std::vector<SurfaceTag> tags;
    };

    std::vector<SimplifyJobInput> inputs;
    inputs.reserve(nodes.size());
    for (auto& node : nodes) {
        if (!node || !node->mesh() || node->mesh()->lods.empty()) continue;

        const auto& lodMesh = node->mesh();
        std::string key = lodMesh->name;
        auto stateIt = m_partSimplifyStates.find(key);

        const MeshData* source = nullptr;
        if (stateIt != m_partSimplifyStates.end()) {
            source = &stateIt->second.originalMesh;
        } else {
            source = &lodMesh->lods[0].mesh;
        }
        if (!source || source->triangleCount() == 0) {
            MF_WARN("Skipping '{}' — source mesh has 0 triangles", node->name());
            continue;
        }

        SimplifyJobInput input;
        input.shapeKey = key;
        input.nodeName = node->name();
        input.source = *source;
        if (m_config.useCADAwarePipeline) {
            auto tagsIt = m_taggedMeshes.find(key);
            if (tagsIt != m_taggedMeshes.end()) {
                input.tags = tagsIt->second.triangleTags;
            }
        }
        inputs.push_back(std::move(input));
    }

    if (inputs.empty()) {
        MF_WARN("No valid meshes selected for simplification");
        return;
    }

    auto task = std::make_shared<Task>("Simplify Selected", [this, inputs = std::move(inputs), params](Task* t) {
        t->setProgress(0.0f); t->setMessage("Simplifying selected parts...");

        size_t total = inputs.size();
        size_t processed = 0;
        size_t totalBefore = 0, totalAfter = 0;
        std::vector<PendingSimplifyResult> results;
        results.reserve(inputs.size());

        for (const auto& input : inputs) {
            // Compute the effective ratio (relative to original)
            float effectiveRatio = params.targetRatio;
            if (params.mode == SimplifyMode::TargetFileSizeMB) {
                effectiveRatio = ratioForTargetFileSize(input.source, params.targetFileSizeMB);
                MF_INFO("FileSize mode: targetMB={:.2f} -> ratio={:.3f} for '{}'",
                        params.targetFileSizeMB, effectiveRatio, input.nodeName);
            }

            // Clamp and validate
            effectiveRatio = std::clamp(effectiveRatio, 0.005f, 1.0f);
            if (effectiveRatio >= 1.0f) {
                MF_INFO("Part '{}' effective ratio {:.3f} >= 1.0, skipping", input.nodeName, effectiveRatio);
                ++processed;
                continue;
            }

            t->setMessage("Simplifying: " + input.nodeName);
            totalBefore += input.source.triangleCount();

            // Run simplification
            SimplifyResult sr;
            if (m_config.useCADAwarePipeline) {
                SimplifyParams sp = params;
                sp.targetRatio = effectiveRatio;

                if (!input.tags.empty()) {
                    sr = m_featureSimplifier.simplifyV2(input.source, sp, &input.tags);
                } else {
                    sr = m_featureSimplifier.simplifyV2(input.source, sp, nullptr);
                }
            } else {
                SimplifyParams sp = params;
                sp.targetRatio = effectiveRatio;
                sr = Simplifier::simplify(input.source, sp);
            }

            sr.mesh.computeNormals();
            sr.mesh.computeTangents();
            totalAfter += sr.mesh.triangleCount();

            MF_INFO("Part '{}' simplified: {} -> {} tris (target {:.3f}, achieved {:.3f}, "
                    "error {:.4f}, sloppy={}, passes={})",
                    input.nodeName, input.source.triangleCount(), sr.mesh.triangleCount(),
                    effectiveRatio, sr.achievedRatio,
                    sr.usedError, sr.usedSloppy, sr.passes);

            PendingSimplifyResult pending;
            pending.shapeKey = input.shapeKey;
            pending.mesh = std::move(sr.mesh);
            results.push_back(std::move(pending));

            ++processed;
            t->setProgress(static_cast<float>(processed) / static_cast<float>(total));
        }

        t->setProgress(0.95f);
        {
            std::lock_guard<std::mutex> lock(m_pendingSimplifyMutex);
            for (auto& result : results) {
                m_pendingSimplifyResults.push_back(std::move(result));
            }
        }
        t->setProgress(1.0f);
        MF_INFO("Simplification complete: {} parts, {} -> {} tris ({:.1f}% reduction)",
                processed, totalBefore, totalAfter,
                totalBefore > 0 ? (1.0f - static_cast<float>(totalAfter) / static_cast<float>(totalBefore)) * 100.0f : 0.0f);
    });

    m_processing = true;
    TaskSystem::instance().submit(task);
}

void Application::rebuildViewportMesh() {
    if (!m_stepResult) return;

    m_viewportMesh.clear();
    m_partIndexRanges.clear();
    for (auto& [id, part] : m_stepResult->partsById) {
        auto meshIt = m_partMeshes.find(part->shapeKey);
        if (meshIt == m_partMeshes.end()) continue;

        Mat4 world(1.0f);
        auto p = part;
        while (p && p->type != AssemblyNode::Type::Root) {
            world = p->localTransform * world;
            p = p->parent;
        }

        const MeshData* localMesh = nullptr;
        auto stateIt = m_partSimplifyStates.find(part->shapeKey);
        if (stateIt != m_partSimplifyStates.end() && stateIt->second.isSimplified)
            localMesh = &stateIt->second.simplifiedMesh;
        else
            localMesh = &meshIt->second->lods[0].mesh;

        if (localMesh->vertexCount() == 0) continue;

        uint32_t vertOffset = static_cast<uint32_t>(m_viewportMesh.vertices.size());
        uint32_t idxOffset  = static_cast<uint32_t>(m_viewportMesh.indices.size());

        for (const auto& v : localMesh->vertices) {
            Vertex wv = v;
            wv.position = Vec3(world * Vec4(v.position, 1.0f));
            wv.normal = glm::normalize(Vec3(world * Vec4(v.normal, 0.0f)));
            wv.uv = v.uv;
            wv.tangent = v.tangent;
            m_viewportMesh.vertices.push_back(wv);
        }
        for (auto idx : localMesh->indices) {
            m_viewportMesh.indices.push_back(vertOffset + idx);
        }

        uint32_t idxCount = static_cast<uint32_t>(m_viewportMesh.indices.size()) - idxOffset;
        m_partIndexRanges[id] = {idxOffset, idxCount};
    }

    if (!m_viewportMesh.vertices.empty()) {
        m_viewportMesh.computeAABB();
        m_ui.setMeshForViewport(&m_viewportMesh);

        uint32_t totalVerts = m_viewportMesh.vertexCount();
        uint32_t totalIndices = m_viewportMesh.indexCount();

        std::vector<ViewportPanel::PartPickInfo> pickParts;
        for (auto& [pid, range] : m_partIndexRanges) {
            AABB aabb;
            for (uint32_t i = range.first; i < range.first + range.second && i < totalIndices; ++i) {
                uint32_t vi = m_viewportMesh.indices[i];
                if (vi < totalVerts) aabb.expand(m_viewportMesh.vertices[vi].position);
            }
            ViewportPanel::PartPickInfo pi;
            pi.indexOffset = range.first;
            pi.indexCount = range.second;
            pi.worldAABB = aabb;
            auto nit = m_partNodeIds.find(pid);
            if (nit != m_partNodeIds.end()) pi.sceneNodeId = nit->second;
            auto pit = m_stepResult->partsById.find(pid);
            if (pit != m_stepResult->partsById.end()) pi.meshName = pit->second->shapeKey;
            pickParts.push_back(std::move(pi));
        }
        m_ui.setViewportPickParts(pickParts);
    }
}

void Application::undoSpecific(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    if (nodes.empty()) return;

    auto task = std::make_shared<Task>("Undo Simplify", [this, nodes](Task* t) {
        t->setProgress(0.0f); t->setMessage("Undoing simplification...");
        size_t undone = 0;
        for (auto& node : nodes) {
            if (!node->mesh() || node->mesh()->lods.empty()) continue;
            std::string key = node->mesh()->name;

            auto stateIt = m_partSimplifyStates.find(key);
            if (stateIt == m_partSimplifyStates.end()) continue;
            if (stateIt->second.history.empty()) {
                MF_INFO("No history for '{}', cannot undo", node->name());
                continue;
            }

            // Pop most recent from history and restore
            stateIt->second.simplifiedMesh = std::move(stateIt->second.history.back());
            stateIt->second.history.pop_back();

            auto meshIt = m_partMeshes.find(key);
            if (meshIt != m_partMeshes.end() && !meshIt->second->lods.empty()) {
                meshIt->second->lods[0].mesh = stateIt->second.simplifiedMesh;
            }
            ++undone;
            MF_INFO("Undo for '{}': restored to {} tris (history remaining: {})",
                    node->name(), stateIt->second.simplifiedMesh.triangleCount(),
                    stateIt->second.history.size());
        }
        m_viewportMeshDirty = true;
        t->setProgress(1.0f);
        MF_INFO("Undo: {} parts restored", undone);
    });
    TaskSystem::instance().submit(task);
}

// ------------------------------------------------------------------
// Bounding proxy: replace mesh with simplified bounding geometry
// ------------------------------------------------------------------
void Application::applyBoundingProxy(const std::vector<std::shared_ptr<SceneNode>>& nodes,
                                      float margin, int geomType) {
    if (nodes.empty()) return;

    for (auto& node : nodes) {
        if (!node->mesh() || node->mesh()->lods.empty()) continue;
        const auto& lodMesh = node->mesh();
        std::string key = lodMesh->name;
        const auto& source = lodMesh->lods[0].mesh;
        if (source.vertexCount() == 0) continue;

        // Compute world AABB from current mesh vertices (already in world space if this is the viewport copy)
        // Use the part index range AABB or the mesh AABB
        AABB aabb = source.aabb;
        if (aabb.isEmpty()) continue;

        // Apply margin
        Vec3 center = aabb.center();
        Vec3 halfExt = aabb.extent() * 0.5f * (1.0f + margin);
        AABB expandedAABB;
        expandedAABB.min = center - halfExt;
        expandedAABB.max = center + halfExt;

        // Generate proxy mesh
        MeshData proxy;
        if (geomType == 0) { // Box
            // 8 vertices + 12 triangles
            Vec3 corners[8] = {
                {expandedAABB.min.x, expandedAABB.min.y, expandedAABB.min.z},
                {expandedAABB.max.x, expandedAABB.min.y, expandedAABB.min.z},
                {expandedAABB.max.x, expandedAABB.max.y, expandedAABB.min.z},
                {expandedAABB.min.x, expandedAABB.max.y, expandedAABB.min.z},
                {expandedAABB.min.x, expandedAABB.min.y, expandedAABB.max.z},
                {expandedAABB.max.x, expandedAABB.min.y, expandedAABB.max.z},
                {expandedAABB.max.x, expandedAABB.max.y, expandedAABB.max.z},
                {expandedAABB.min.x, expandedAABB.max.y, expandedAABB.max.z}};
            for (int i = 0; i < 8; ++i) {
                Vertex v; v.position = corners[i]; v.normal = Vec3(0,1,0); v.uv = Vec2(0,0); v.tangent = Vec4(1,0,0,1);
                proxy.vertices.push_back(v);
            }
            int faces[12][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                               {2,3,7},{2,7,6},{1,2,6},{1,6,5},{3,0,4},{3,4,7}};
            for (auto& f : faces) {
                proxy.indices.push_back(f[0]); proxy.indices.push_back(f[1]); proxy.indices.push_back(f[2]);
            }
            proxy.computeNormals();
        } else if (geomType == 1) { // Sphere
            uint32_t slices = 16, stacks = 8;
            for (uint32_t si = 0; si <= stacks; ++si) {
                float phi = 3.14159f * si / stacks;
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    float theta = 2.0f * 3.14159f * ci / slices;
                    Vec3 n(std::sin(phi)*std::cos(theta), std::cos(phi), std::sin(phi)*std::sin(theta));
                    Vec3 p = center + halfExt * n;
                    Vertex v; v.position = p; v.normal = n; v.uv = Vec2((float)ci/slices, (float)si/stacks);
                    v.tangent = Vec4(1,0,0,1); proxy.vertices.push_back(v);
                }
            }
            for (uint32_t si = 0; si < stacks; ++si)
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    uint32_t a = si*slices+ci, b = si*slices+(ci+1)%slices;
                    uint32_t c = (si+1)*slices+ci, d = (si+1)*slices+(ci+1)%slices;
                    proxy.indices.push_back(a);proxy.indices.push_back(b);proxy.indices.push_back(c);
                    proxy.indices.push_back(b);proxy.indices.push_back(d);proxy.indices.push_back(c);
                }
        } else { // Cylinder
            uint32_t slices = 16;
            Vec3 axis(0,1,0);
            Vec3 u(1,0,0), v(0,0,1);
            float r = std::max(halfExt.x, halfExt.z);
            float h = halfExt.y * 2.0f;
            Vec3 base = center - Vec3(0, halfExt.y, 0);
            for (uint32_t si = 0; si <= 1; ++si) {
                Vec3 c = base + axis * (si * h);
                for (uint32_t ci = 0; ci < slices; ++ci) {
                    float angle = 2.0f*3.14159f*ci/slices;
                    Vec3 p = c + r*(u*std::cos(angle)+v*std::sin(angle));
                    Vec3 n = glm::normalize(u*std::cos(angle)+v*std::sin(angle));
                    Vertex vt; vt.position = p; vt.normal = n;
                    vt.uv = Vec2((float)ci/slices, (float)si); vt.tangent = Vec4(1,0,0,1);
                    proxy.vertices.push_back(vt);
                }
            }
            for (uint32_t ci = 0; ci < slices; ++ci) {
                uint32_t a=ci, b=(ci+1)%slices, c=slices+ci, d=slices+(ci+1)%slices;
                proxy.indices.push_back(a);proxy.indices.push_back(b);proxy.indices.push_back(c);
                proxy.indices.push_back(b);proxy.indices.push_back(d);proxy.indices.push_back(c);
            }
        }
        proxy.computeAABB();

        // Replace the part's mesh
        auto stateIt = m_partSimplifyStates.find(key);
        if (stateIt != m_partSimplifyStates.end()) {
            stateIt->second.originalMesh = proxy;
            stateIt->second.simplifiedMesh = proxy;
            stateIt->second.isSimplified = true;
        }
        auto meshIt = m_partMeshes.find(key);
        if (meshIt != m_partMeshes.end() && !meshIt->second->lods.empty()) {
            meshIt->second->lods[0].mesh = proxy;
        }
    }

    rebuildViewportMesh();
    MF_INFO("Bounding proxy applied to {} parts", nodes.size());
}

void Application::resetToOriginal(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    if (nodes.empty()) return;

    auto task = std::make_shared<Task>("Reset to Original", [this, nodes](Task* t) {
        t->setProgress(0.0f); t->setMessage("Resetting to original...");
        size_t reset = 0;
        for (auto& node : nodes) {
            if (!node->mesh() || node->mesh()->lods.empty()) continue;
            std::string key = node->mesh()->name;

            auto stateIt = m_partSimplifyStates.find(key);
            if (stateIt == m_partSimplifyStates.end()) continue;
            if (!stateIt->second.isSimplified) continue;

            stateIt->second.simplifiedMesh = MeshData{};
            stateIt->second.isSimplified = false;
            stateIt->second.history.clear();

            auto meshIt = m_partMeshes.find(key);
            if (meshIt != m_partMeshes.end() && !meshIt->second->lods.empty()) {
                meshIt->second->lods[0].mesh = stateIt->second.originalMesh;
            }
            ++reset;
        }
        m_viewportMeshDirty = true;
        t->setProgress(1.0f);
        MF_INFO("Reset: {} parts restored to original", reset);
    });
    TaskSystem::instance().submit(task);
}

void Application::batchExportSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    // Each selected node's IMMEDIATE children are exported as separate files.
    // If a child is a Group (sub-assembly), it is exported as a whole merged file
    // (its grandchildren are NOT further split).

    if (nodes.empty()) {
        MF_WARN("No nodes selected for batch export");
        return;
    }

    // Get a directory
    std::string dirPath = m_ui.saveFileDialog(false);
    if (dirPath.empty()) return;

    std::string baseDir = dirPath.substr(0, dirPath.find_last_of('.'));
    if (baseDir.empty()) baseDir = dirPath + "_parts";

    std::string mkdir_cmd = "mkdir -p \"" + baseDir + "\"";
    system(mkdir_cmd.c_str());

    STLExporter stlExporter;
    size_t exported = 0;

    for (auto& node : nodes) {
        // For each selected node, iterate its IMMEDIATE children only
        for (auto& child : node->children()) {
            // Merge this child + all its descendants into one mesh
            MeshData merged;
            child->traverse([&merged](SceneNode* descendant) {
                if (descendant->type() == SceneNode::Type::Mesh && descendant->mesh()
                    && !descendant->mesh()->lods.empty()) {
                    const auto& md = descendant->mesh()->lods[0].mesh;
                    if (md.vertices.empty()) return;

                    uint32_t off = static_cast<uint32_t>(merged.vertices.size());
                    Mat4 w = descendant->worldTransform();
                    for (const auto& v : md.vertices) {
                        Vertex wv = v;
                        wv.position = Vec3(w * Vec4(v.position, 1.0f));
                        wv.normal  = glm::normalize(Vec3(w * Vec4(v.normal, 0.0f)));
                        merged.vertices.push_back(wv);
                    }
                    for (auto idx : md.indices) {
                        merged.indices.push_back(off + idx);
                    }
                }
            });

            if (merged.vertices.empty()) continue;
            merged.computeAABB();

            std::string safeName = child->name();
            for (auto& c : safeName) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                    c = '_';
            }
            if (safeName.empty()) safeName = "part";

            std::string stlPath = baseDir + "/" + safeName + ".stl";
            if (stlExporter.exportMesh(merged, stlPath, safeName)) {
                MF_INFO("  Exported '{}' ({} tris) -> {}", child->name(),
                        merged.triangleCount(), stlPath);
                ++exported;
            }
        }
    }

    MF_INFO("Batch export: {} parts written to '{}'", exported, baseDir);
}

// NOTE: Runs synchronously on the main thread to avoid data races
// between background task workers and the UI thread.
void Application::deleteSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    if (nodes.empty() || !m_stepResult || !m_scene) return;

    MF_INFO("Deleting {} selected parts...", nodes.size());

    std::unordered_set<std::string> removedPartIds;
    std::unordered_set<std::string> candidateShapeKeys;

    for (auto& node : nodes) {
        if (!node) continue;

        std::string key = node->mesh() ? node->mesh()->name : "";
        for (auto& [partId, part] : m_stepResult->partsById) {
            if (part->shapeKey == key) {
                removedPartIds.insert(partId);
                candidateShapeKeys.insert(key);
                break;
            }
        }

        // Remove from scene tree
        if (auto parent = node->parent()) {
            parent->removeChild(node);
        }
    }

    // Erase removed parts
    for (auto& pid : removedPartIds) {
        m_stepResult->partsById.erase(pid);
        m_partIndexRanges.erase(pid);
        m_partNodeIds.erase(pid);
    }

    // Clean up orphaned shapes
    for (auto& sk : candidateShapeKeys) {
        bool stillUsed = false;
        for (auto& [pid, p] : m_stepResult->partsById) {
            if (p->shapeKey == sk) { stillUsed = true; break; }
        }
        if (!stillUsed) {
            m_partMeshes.erase(sk);
            m_partSimplifyStates.erase(sk);
            m_taggedMeshes.erase(sk);
            m_stepResult->shapesByKey.erase(sk);
        }
    }

    // Defer viewport rebuild — modifying OpenGL buffers during the current
    // frame's draw would overwrite data the GPU is still processing.
    m_viewportMeshDirty = true;

    MF_INFO("Deleted {} parts", removedPartIds.size());
}

// ------------------------------------------------------------------
// Group: merge selected parts into a new Group node
// ------------------------------------------------------------------
void Application::groupSelected(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    if (nodes.size() < 2) {
        MF_WARN("Need at least 2 nodes to group");
        return;
    }
    if (!m_scene) return;

    // Find common parent
    auto commonParent = nodes[0]->parent();
    if (!commonParent) commonParent = m_scene->root();
    for (size_t i = 1; i < nodes.size(); ++i) {
        if (nodes[i]->parent() != commonParent) {
            MF_WARN("Selected nodes must share the same parent");
            return;
        }
    }

    auto task = std::make_shared<Task>("Group Parts", [this, nodes, commonParent](Task* t) {
        t->setProgress(0.0f); t->setMessage("Merging parts...");

        // Create merged group node
        auto groupNode = std::make_shared<SceneNode>("MergedGroup", SceneNode::Type::Group);
        groupNode->setLocalTransform(mat4ToTransform(Mat4(1.0f)));

        // Create merged mesh from all selected nodes
        MeshData mergedMesh;
        for (auto& node : nodes) {
            if (!node->mesh() || node->mesh()->lods.empty()) continue;
            const auto& md = node->mesh()->lods[0].mesh;
            Mat4 world = node->worldTransform();

            uint32_t off = static_cast<uint32_t>(mergedMesh.vertices.size());
            for (const auto& v : md.vertices) {
                Vertex wv = v;
                wv.position = Vec3(world * Vec4(v.position, 1.0f));
                wv.normal = glm::normalize(Vec3(world * Vec4(v.normal, 0.0f)));
                mergedMesh.vertices.push_back(wv);
            }
            for (auto idx : md.indices) {
                mergedMesh.indices.push_back(off + idx);
            }
        }
        mergedMesh.computeAABB();

        t->setProgress(0.5f); t->setMessage("Building scene structure...");

        // Attach merged mesh to group node
        auto mergedLOD = std::make_shared<LODMesh>();
        mergedLOD->name = "MergedGroup_mesh";
        LODLevel l0;
        l0.level = 0;
        l0.mesh = std::move(mergedMesh);
        mergedLOD->lods.push_back(std::move(l0));
        groupNode->setMesh(mergedLOD);

        // Store in part meshes for viewport
        m_partMeshes["MergedGroup_mesh"] = mergedLOD;

        // Move selected nodes as children of the group
        for (auto& node : nodes) {
            commonParent->removeChild(node);
            groupNode->addChild(node);
        }

        // Add group to common parent
        commonParent->addChild(groupNode);

        t->setProgress(0.8f); t->setMessage("Rebuilding viewport...");
        m_viewportMeshDirty = true;
        t->setProgress(1.0f);
        MF_INFO("Grouped {} parts into MergedGroup", nodes.size());
    });
    TaskSystem::instance().submit(task);
}

void Application::exportglTF(const std::string& filepath) {
    glTFExporter exporter;
    if (exporter.exportScene(*m_scene, filepath, m_config.exportOpt)) {
        MF_INFO("Exported to {}", filepath);
    } else {
        MF_ERROR("Export failed");
    }
}

void Application::exportSTL(const std::string& filepath) {
    STLExporter exporter;
    if (exporter.exportScene(*m_scene, filepath)) {
        MF_INFO("Exported STL to {}", filepath);
    } else {
        MF_ERROR("STL Export failed");
    }
}

bool Application::isProcessing() const { return m_processing; }
float Application::progress() const { return m_progress; }

} // namespace mf
