#include "imgui_bgfx_backend.h"
#include <bgfx/embedded_shader.h>
#include <bx/math.h>

// Pre-compiled shader binaries from bgfx examples (Metal-compatible)
#include "../bgfx/examples/common/imgui/vs_ocornut_imgui.bin.h"
#include "../bgfx/examples/common/imgui/fs_ocornut_imgui.bin.h"

static bgfx::ProgramHandle s_imguiProgram = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
static bgfx::TextureHandle s_fontTexture = BGFX_INVALID_HANDLE;
static bgfx::VertexLayout s_imguiLayout;

static const bgfx::EmbeddedShader s_embeddedShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER_END()
};

void imguiCreate(float fontSize) {
    (void)fontSize;

    s_imguiLayout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
        .end();

    bgfx::RendererType::Enum type = bgfx::getRendererType();
    s_imguiProgram = bgfx::createProgram(
        bgfx::createEmbeddedShader(s_embeddedShaders, type, "vs_ocornut_imgui"),
        bgfx::createEmbeddedShader(s_embeddedShaders, type, "fs_ocornut_imgui"),
        true);

    s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height, bpp;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);

    s_fontTexture = bgfx::createTexture2D(
        uint16_t(width), uint16_t(height), false, 1,
        bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_NONE,
        bgfx::copy(pixels, uint32_t(width * height * 4)));

    io.Fonts->TexID = (ImTextureID)(intptr_t)s_fontTexture.idx;
}

void imguiDestroy() {
    if (bgfx::isValid(s_imguiProgram)) bgfx::destroy(s_imguiProgram);
    if (bgfx::isValid(s_tex)) bgfx::destroy(s_tex);
    if (bgfx::isValid(s_fontTexture)) bgfx::destroy(s_fontTexture);
}

void imguiBeginFrame(int32_t mx, int32_t my, uint8_t button, int32_t scroll,
                     uint16_t width, uint16_t height, int inputChar, bgfx::ViewId viewId) {
    (void)mx; (void)my; (void)button; (void)scroll; (void)inputChar;
    (void)width; (void)height; (void)viewId;
    // View setup done in imguiEndFrame render(), matching bgfx example pattern
}

void imguiEndFrame() {
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->CmdListsCount == 0) return;

    const bgfx::ViewId viewId = 255;
    const bgfx::Caps* caps = bgfx::getCaps();

    // Set ortho projection exactly like bgfx example
    {
        float ortho[16];
        float x = drawData->DisplayPos.x;
        float y = drawData->DisplayPos.y;
        float w = drawData->DisplaySize.x;
        float h = drawData->DisplaySize.y;
        bx::mtxOrtho(ortho, x, x + w, y + h, y, 0.0f, 1000.0f, 0.0f, caps->homogeneousDepth);
        bgfx::setViewTransform(viewId, nullptr, ortho);
        bgfx::setViewRect(viewId, 0, 0, uint16_t(w), uint16_t(h));
    }

    const ImVec2 clipPos   = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int ii = 0; ii < drawData->CmdListsCount; ++ii) {
        const ImDrawList* drawList = drawData->CmdLists[ii];
        uint32_t numVertices = uint32_t(drawList->VtxBuffer.size());
        uint32_t numIndices  = uint32_t(drawList->IdxBuffer.size());

        if (numVertices == 0 || numIndices == 0) continue;

        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;

        if (numVertices != bgfx::getAvailTransientVertexBuffer(numVertices, s_imguiLayout)) {
            break; // not enough transient buffer space
        }
        bgfx::allocTransientVertexBuffer(&tvb, numVertices, s_imguiLayout);

        if (numIndices != bgfx::getAvailTransientIndexBuffer(numIndices, sizeof(ImDrawIdx) == 4)) {
            break;
        }
        bgfx::allocTransientIndexBuffer(&tib, numIndices, sizeof(ImDrawIdx) == 4);

        bx::memCopy(tvb.data, drawList->VtxBuffer.begin(), numVertices * sizeof(ImDrawVert));
        bx::memCopy(tib.data, drawList->IdxBuffer.begin(), numIndices * sizeof(ImDrawIdx));

        bgfx::Encoder* encoder = bgfx::begin();

        for (const ImDrawCmd* cmd = drawList->CmdBuffer.begin(), *cmdEnd = drawList->CmdBuffer.end();
             cmd != cmdEnd; ++cmd) {
            if (cmd->UserCallback) {
                cmd->UserCallback(drawList, cmd);
            } else if (cmd->ElemCount > 0) {
                uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA;

                bgfx::TextureHandle th = s_fontTexture;
                bgfx::ProgramHandle program = s_imguiProgram;

                if (cmd->GetTexID()) {
                    th = { uint16_t(intptr_t(cmd->GetTexID())) };
                }
                state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);

                // Project clip rect into framebuffer space
                ImVec4 clipRect;
                clipRect.x = (cmd->ClipRect.x - clipPos.x) * clipScale.x;
                clipRect.y = (cmd->ClipRect.y - clipPos.y) * clipScale.y;
                clipRect.z = (cmd->ClipRect.z - clipPos.x) * clipScale.x;
                clipRect.w = (cmd->ClipRect.w - clipPos.y) * clipScale.y;

                if (clipRect.x < drawData->DisplaySize.x * clipScale.x &&
                    clipRect.y < drawData->DisplaySize.y * clipScale.y &&
                    clipRect.z >= 0.0f &&
                    clipRect.w >= 0.0f) {
                    uint16_t xx = uint16_t(bx::max(clipRect.x, 0.0f));
                    uint16_t yy = uint16_t(bx::max(clipRect.y, 0.0f));
                    encoder->setScissor(xx, yy,
                        uint16_t(bx::min(clipRect.z, 65535.0f) - xx),
                        uint16_t(bx::min(clipRect.w, 65535.0f) - yy));

                    encoder->setState(state);
                    encoder->setTexture(0, s_tex, th);
                    encoder->setVertexBuffer(0, &tvb, cmd->VtxOffset, numVertices);
                    encoder->setIndexBuffer(&tib, cmd->IdxOffset, cmd->ElemCount);
                    encoder->submit(viewId, program);
                }
            }
        }

        bgfx::end(encoder);
    }
}
