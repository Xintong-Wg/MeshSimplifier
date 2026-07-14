#pragma once

#include "Core/Types.h"
#include "Core/TaskSystem.h"
#include "Scene/SceneGraph.h"
#include "Mesh/Simplifier.h"
#include <imgui.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_set>
#include <functional>

struct SDL_Window;
using SDL_GLContext = void*;

namespace mf {

// ------------------------------------------------------------------
// Context menu action callbacks (set by Application)
// ------------------------------------------------------------------
struct ActionCallbacks {
    std::function<void()> onDeleteSelected;
    std::function<void()> onExportSelectedglTF;
    std::function<void()> onExportSelectedSTL;
    std::function<void()> onGroupSelected;
    std::function<void()> onFocusOnSelection;
};

class Panel {
public:
    virtual ~Panel() = default;
    virtual const char* name() const = 0;
    virtual void draw() = 0;
    virtual ImGuiWindowFlags windowFlags() const { return ImGuiWindowFlags_None; }
    bool visible() const { return m_visible; }
    void setVisible(bool v) { m_visible = v; }
    void setActionCallbacks(ActionCallbacks* cb) { m_actionCb = cb; }
protected:
    bool m_visible = true;
    ActionCallbacks* m_actionCb = nullptr;
    ActionCallbacks* getActionCbs() const { return m_actionCb; }
};

// ------------------------------------------------------------------
// Scene Tree panel
// ------------------------------------------------------------------
class SceneTreePanel : public Panel {
public:
    const char* name() const override { return "Scene Tree"; }
    void draw() override;
    void setScene(std::shared_ptr<Scene> scene) { m_scene = scene; }

    // Multi-select: get all selected nodes
    std::vector<std::shared_ptr<SceneNode>> selectedNodes() const;
    // Single-select: first selected node (backwards compat)
    std::shared_ptr<SceneNode> selectedNode() const;
    // Selection set
    const std::unordered_set<EntityId>& selection() const { return m_selection; }
    // Set selection from external source (viewport pick)
    void setSelection(const std::unordered_set<EntityId>& ids);
private:
    void drawNode(SceneNode* node);
    void collectVisibleNodes(SceneNode* root, std::vector<SceneNode*>& flatList);
    void selectRange(SceneNode* from, SceneNode* to,
                     std::vector<SceneNode*>& flatList);
    void collectAncestors(SceneNode* node, std::unordered_set<EntityId>& ancestors);

    std::shared_ptr<Scene> m_scene;
    std::vector<std::shared_ptr<SceneNode>> m_selectedNodes;
    std::unordered_set<EntityId> m_selection;
    std::unordered_set<EntityId> m_forceExpand;  // auto-expand ancestors on selection
    EntityId m_scrollToNode = 0;  // scroll to this node on next draw
    EntityId m_lastClickedNode = 0;  // for Shift-click range selection
};

// ------------------------------------------------------------------
// Property panel
// ------------------------------------------------------------------
class PropertyPanel : public Panel {
public:
    const char* name() const override { return "Properties"; }
    void draw() override;
    void setNode(std::shared_ptr<SceneNode> node) { m_node = node; }
private:
    std::shared_ptr<SceneNode> m_node;
};

// ------------------------------------------------------------------
// Task System panel
// ------------------------------------------------------------------
class TaskPanel : public Panel {
public:
    const char* name() const override { return "Tasks"; }
    void draw() override;
};

// ------------------------------------------------------------------
// Stats panel
// ------------------------------------------------------------------
class StatsPanel : public Panel {
public:
    const char* name() const override { return "Stats"; }
    void draw() override;
    void setStats(uint32_t triangles, uint32_t drawCalls, float fps) {
        m_triangles = triangles; m_drawCalls = drawCalls; m_fps = fps;
    }
    void setPipeline(int analyzed, int classified, int tessellated,
                     int shellBefore, int shellAfter, bool cadAware) {
        m_pipeAnalyzed = analyzed; m_pipeClassified = classified;
        m_pipeTessellated = tessellated;
        m_pipeShellBefore = shellBefore; m_pipeShellAfter = shellAfter;
        m_pipeCADAware = cadAware;
    }
private:
    uint32_t m_triangles = 0;
    uint32_t m_drawCalls = 0;
    float m_fps = 0;
    int m_pipeAnalyzed = 0, m_pipeClassified = 0, m_pipeTessellated = 0;
    int m_pipeShellBefore = 0, m_pipeShellAfter = 0;
    bool m_pipeCADAware = false;
};

// ------------------------------------------------------------------
// 3D Viewport panel (OpenGL FBO-based)
// ------------------------------------------------------------------
class ViewportPanel : public Panel {
public:
    const char* name() const override { return "Viewport"; }
    ViewportPanel();
    ~ViewportPanel();
    void draw() override;
    ImGuiWindowFlags windowFlags() const override { return ImGuiWindowFlags_NoMove; }
    void initGL();
    void setMesh(const MeshData* mesh) { m_mesh = mesh; m_meshDirty = true; }
    void clearMesh() { m_mesh = nullptr; m_meshDirty = true; m_gpuIndexCount = 0; }
    const MeshData* getMesh() const { return m_mesh; }
    void focusOnObject();
    void refresh();  // force re-upload mesh to GPU
    void processInput();  // handle camera + mouse pick (call before draw)

    // Highlight
    void setHighlightRange(uint32_t indexOffset, uint32_t indexCount);
    void setHighlightRanges(const std::vector<std::pair<uint32_t,uint32_t>>& ranges);
    void clearHighlight();
    const std::vector<std::pair<uint32_t,uint32_t>>& selectedFaceRanges() const { return m_selectedFaceRanges; }
    void clearSelectedFaces() { m_selectedFaceRanges.clear(); }

    // Per-part info for viewport selection (point / box pick)
    struct PartPickInfo {
        EntityId sceneNodeId = 0;
        std::string meshName;
        uint32_t indexOffset;
        uint32_t indexCount;
        AABB worldAABB;
    };
    void setPickParts(const std::vector<PartPickInfo>& parts);

    // Selection mode
    enum class SelectionMode { Part, MeshFace };
    void setSelectionMode(SelectionMode m) { m_selMode = m; }
    SelectionMode selectionMode() const { return m_selMode; }

    // Selection callback: viewport -> scene tree
    using SelectionCallback = std::function<void(const std::vector<EntityId>&)>;
    void setSelectionCallback(SelectionCallback cb) { m_selectionCallback = cb; }

private:
    void uploadMesh();
    void render3D();
    void orbitCamera();

    // OpenGL resources
    unsigned m_fbo = 0, m_fboTex = 0, m_fboDepth = 0;
    unsigned m_vao = 0, m_vbo = 0, m_ebo = 0;
    unsigned m_shader = 0;
    int m_fboW = 1024, m_fboH = 768;

    // Mesh data
    const MeshData* m_mesh = nullptr;
    bool m_meshDirty = true;
    uint32_t m_gpuIndexCount = 0;

    // Highlight state (index range in the merged mesh)
    uint32_t m_highlightIndexOffset = 0;
    uint32_t m_highlightIndexCount = 0;
    bool m_hasHighlight = false;

    // Per-part pick info for viewport selection
    std::vector<PartPickInfo> m_pickParts;
    SelectionCallback m_selectionCallback;

    // Mouse interaction state
    bool m_mouseLeftDown = false;
    bool m_mouseRightDown = false;
    bool m_isBoxSelecting = false;
    float m_boxSelectStartX = 0, m_boxSelectStartY = 0;
    float m_boxSelectEndX = 0, m_boxSelectEndY = 0;

    // FBO image position (screen coords) — set in render3D, used in pick
    float m_vpImgOriginX = 0, m_vpImgOriginY = 0;
    float m_vpImgW = 0, m_vpImgH = 0;

    // Selection state
    SelectionMode m_selMode = SelectionMode::Part;
    std::vector<std::pair<uint32_t, uint32_t>> m_selectedFaceRanges; // {offset, count} pairs

    // Camera orbit state
    float m_yaw = 0.5f, m_pitch = 0.3f, m_distance = 5.0f;
    Vec3 m_target = Vec3(0, 0, 0);
    bool m_dragging = false;
    float m_lastMouseX = 0, m_lastMouseY = 0;
    float m_farPlane = 1000.0f;

    void handleMousePick();
    void doRayPick(float mouseX, float mouseY);
    void doBoxPick(float x0, float y0, float x1, float y1);
    void drawSelectionBox();
    void drawViewCube();

    // Set camera to a named view direction
    void setViewDirection(const Vec3& dir, const Vec3& up);
};

// ------------------------------------------------------------------
// Proxy Simplify panel — bounding geometry replacement
// ------------------------------------------------------------------
class ProxySimplifyPanel : public Panel {
public:
    const char* name() const override { return "Proxy Simplify"; }
    void draw() override;
    void setTargetNodes(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    const std::vector<std::shared_ptr<SceneNode>>& targetNodes() const { return m_targetNodes; }
    bool applyRequested() const { return m_applyRequested; }
    void resetApplyFlag() { m_applyRequested = false; }
    float margin() const { return m_margin; }
    int geomType() const { return m_geomType; }  // 0=box, 1=sphere, 2=cylinder
private:
    std::vector<std::shared_ptr<SceneNode>> m_targetNodes;
    float m_margin = 0.0f;
    int m_geomType = 0;
    bool m_applyRequested = false;
};

// ------------------------------------------------------------------
// Simplify Settings panel
// ------------------------------------------------------------------
class SimplifySettingsPanel : public Panel {
public:
    const char* name() const override { return "Simplify Settings"; }
    void draw() override;
    void setTargetNodes(const std::vector<std::shared_ptr<SceneNode>>& nodes);
    const SimplifyParams& params() const { return m_params; }
    SimplifyParams& params() { return m_params; }
    bool applyRequested() const { return m_applyRequested; }
    void resetApplyFlag() { m_applyRequested = false; }
private:
    SimplifyParams m_params;
    std::vector<std::shared_ptr<SceneNode>> m_targetNodes;
    bool m_applyRequested = false;
    int m_modeCombo = 0; // 0 = Ratio, 1 = FileSizeMB
};

// ------------------------------------------------------------------
// Menu action types
// ------------------------------------------------------------------
enum class MenuAction { None, OpenCAD, ExportglTF, ExportSTL, ExportSelectedglTF,
    ExportSelectedSTL, BatchExportSelected, Exit,
    TessellateAll, GenerateLOD, SimplifyAll, UndoSelected, ResetSelected,
    DeleteSelected };

// ------------------------------------------------------------------
// Main UI Manager
// ------------------------------------------------------------------
class UIManager {
public:
    UIManager();
    ~UIManager();

    bool init(SDL_Window* window, SDL_GLContext glContext);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setScene(std::shared_ptr<Scene> scene);
    void setMeshForViewport(const MeshData* mesh);
    void refreshViewport();
    void setHighlightForViewport(uint32_t offset, uint32_t count);
    void clearViewportHighlight();
    void clearViewportFaceRanges();
    ViewportPanel::SelectionMode viewportSelectionMode() const;
    void setViewportPickParts(const std::vector<ViewportPanel::PartPickInfo>& parts);
    void setViewportSelectionCallback(ViewportPanel::SelectionCallback cb);
    void setActionCallbacks(const ActionCallbacks& cb) { m_actionCallbacks = cb; }
    ActionCallbacks& actionCallbacks() { return m_actionCallbacks; }
    void setStats(uint32_t triangles, uint32_t drawCalls, float fps);
    void setPipelineStats(int analyzed, int classified, int tessellated,
                           int shellBefore, int shellAfter, bool cadAware);

    std::shared_ptr<SceneNode> selectedNode() const;
    std::vector<std::shared_ptr<SceneNode>> selectedNodes() const;
    void selectNodesById(const std::vector<EntityId>& ids);
    SimplifySettingsPanel* simplifySettings() const { return m_simplifySettings; }
    ProxySimplifyPanel* proxySimplify() const { return m_proxySimplify; }
    MenuAction showMainMenu();

    std::string openFileDialog();
    std::string saveFileDialog(bool stlMode = false);

private:
    void setupDockingLayout(ImGuiID dockspaceId);

    std::vector<std::unique_ptr<Panel>> m_panels;
    SceneTreePanel* m_sceneTree = nullptr;
    PropertyPanel* m_property = nullptr;
    ViewportPanel* m_viewport = nullptr;
    StatsPanel* m_stats = nullptr;
    SimplifySettingsPanel* m_simplifySettings = nullptr;
    ProxySimplifyPanel* m_proxySimplify = nullptr;
    ActionCallbacks m_actionCallbacks;
    bool m_firstFrame = true;

    // Pipeline stats for display
    int m_pipeAnalyzed = 0, m_pipeClassified = 0, m_pipeTessellated = 0;
    int m_pipeShellBefore = 0, m_pipeShellAfter = 0;
    bool m_pipeCADAware = false;
};

} // namespace mf
