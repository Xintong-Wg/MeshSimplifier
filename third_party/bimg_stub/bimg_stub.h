#pragma once
#include <cstdint>

// Minimal bimg stub - provides symbols bgfx needs without full bimg

namespace bimg {

enum class TextureFormat : uint8_t { Enum = 0, BGRA8 = 1, RGBA8 = 2 };

struct TextureInfo {
    TextureFormat format = TextureFormat::BGRA8;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint16_t numLayers = 1;
    uint8_t numMips = 1;
    bool cubemap = false;
};

struct ImageContainer {
    TextureInfo info;
    void* data = nullptr;
    uint32_t size = 0;
};

struct ImageMip {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t size = 0;
    const void* data = nullptr;
};

namespace bx { struct AllocatorI; struct Error; class WriterI; }

inline bool isCompressed(TextureFormat) { return false; }
inline bool isDepth(TextureFormat) { return false; }
inline const char* getName(TextureFormat) { return "BGRA8"; }
inline uint32_t getBitsPerPixel(TextureFormat) { return 32; }
inline void getBlockInfo(TextureFormat) {}

inline void imageGetSize(TextureInfo*, uint16_t, uint16_t, uint16_t = 1,
    bool = false, bool = false, uint16_t = 1, TextureFormat = TextureFormat::BGRA8) {}

inline bool imageParse(ImageContainer&, const void*, uint32_t, bx::Error*) { return false; }
inline bool imageDecodeToBgra8(bx::AllocatorI*, void*, const void*, uint32_t, uint32_t, uint32_t, TextureFormat) { return false; }
inline void imageGetRawData(const ImageContainer&, uint16_t, uint8_t, const void*, uint32_t, ImageMip&) {}
inline void imageSwizzleBgra8(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t) {}
inline bool imageWriteTga(bx::WriterI*, uint32_t, uint32_t, uint32_t, const void*, bool, bool, bx::Error*) { return false; }

} // namespace bimg
