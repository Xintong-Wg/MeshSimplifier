#pragma once
#include <cstdint>
#include <cstddef>

// Minimal imgui stub - allows compilation without real imgui

struct ImVec2 { float x = 0, y = 0; ImVec2() = default; ImVec2(float _x, float _y) : x(_x), y(_y) {} };
struct ImVec4 { float x = 0, y = 0, z = 0, w = 0; };

using ImGuiID = unsigned int;
using ImGuiWindowFlags = int;
using ImGuiTreeNodeFlags = int;
using ImGuiDockNodeFlags = int;
using ImGuiStyleVar = int;
using ImGuiConfigFlags = int;

enum ImGuiWindowFlags_ {
    ImGuiWindowFlags_None = 0,
    ImGuiWindowFlags_NoTitleBar = 1 << 0,
    ImGuiWindowFlags_NoCollapse = 1 << 5,
    ImGuiWindowFlags_NoResize = 1 << 1,
    ImGuiWindowFlags_NoMove = 1 << 2,
    ImGuiWindowFlags_NoBringToFrontOnFocus = 1 << 17,
    ImGuiWindowFlags_NoNavFocus = 1 << 18,
    ImGuiWindowFlags_MenuBar = 1 << 4,
};

enum ImGuiTreeNodeFlags_ {
    ImGuiTreeNodeFlags_None = 0,
    ImGuiTreeNodeFlags_Selected = 1 << 0,
    ImGuiTreeNodeFlags_OpenOnArrow = 1 << 6,
    ImGuiTreeNodeFlags_Leaf = 1 << 8,
};

enum ImGuiConfigFlags_ {
    ImGuiConfigFlags_None = 0,
    ImGuiConfigFlags_DockingEnable = 1 << 7,
    ImGuiConfigFlags_ViewportsEnable = 1 << 10,
};

enum ImGuiDockNodeFlags_ { ImGuiDockNodeFlags_None = 0 };

enum ImGuiStyleVar_ {
    ImGuiStyleVar_WindowRounding = 4,
    ImGuiStyleVar_WindowBorderSize = 5,
    ImGuiStyleVar_WindowPadding = 7,
};

struct ImGuiIO {
    bool WantCaptureMouse = false;
    bool WantCaptureKeyboard = false;
    int ConfigFlags = 0;
};

struct ImGuiViewport {
    ImVec2 WorkPos;
    ImVec2 WorkSize;
    ImGuiID ID = 0;
};

namespace ImGui {

inline ImGuiIO& GetIO() { static ImGuiIO io; return io; }
inline ImGuiViewport* GetMainViewport() { static ImGuiViewport vp; return &vp; }
inline void CreateContext() {}
inline void DestroyContext() {}
inline void StyleColorsDark() {}
inline void NewFrame() {}
inline void Render() {}

inline void SetNextWindowPos(ImVec2, int = 0, ImVec2 = ImVec2()) {}
inline void SetNextWindowSize(ImVec2, int = 0) {}
inline void SetNextWindowViewport(ImGuiID) {}

inline bool Begin(const char*, bool*, int = 0) { return true; }
inline void End() {}

inline ImGuiID GetID(const char*) { return 0; }
inline void DockSpace(ImGuiID, ImVec2, int = 0) {}

inline bool BeginMenuBar() { return false; }
inline void EndMenuBar() {}
inline bool BeginMenu(const char*, bool = true) { return false; }
inline void EndMenu() {}
inline bool MenuItem(const char*, const char* = nullptr, bool* = nullptr, bool = true) { return false; }
inline void Separator() {}
inline void Text(const char*, ...) {}
inline void ProgressBar(float, ImVec2 = ImVec2(-1, 0), const char* = nullptr) {}

inline bool TreeNodeEx(const char*, int = 0) { return false; }
inline void TreePop() {}
inline bool IsItemClicked(int = 0) { return false; }

inline bool DragFloat3(const char*, float*, float = 1.0f, float = 0.0f, float = 0.0f, const char* = "%.3f", int = 0) { return false; }

inline void PushStyleVar(int, float) {}
inline void PushStyleVar(int, ImVec2) {}
inline void PopStyleVar(int = 1) {}

} // namespace ImGui

#define IMGUI_CHECKVERSION()
