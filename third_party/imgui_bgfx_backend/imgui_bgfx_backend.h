#pragma once

#include <cstdint>
#include <bgfx/bgfx.h>
#include <imgui.h>

// Minimal bgfx backend for ImGui rendering
// Provides imguiCreate / imguiBeginFrame / imguiEndFrame / imguiDestroy

void imguiCreate(float fontSize = 18.0f);
void imguiDestroy();
void imguiBeginFrame(int32_t mx, int32_t my, uint8_t button, int32_t scroll,
                     uint16_t width, uint16_t height, int inputChar = -1,
                     bgfx::ViewId viewId = 255);
void imguiEndFrame();
