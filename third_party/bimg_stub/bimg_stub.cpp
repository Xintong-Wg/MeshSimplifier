// Minimal bimg stub - provides symbols bgfx needs
#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <bx/error.h>

namespace bx { struct WriterI; }

namespace bimg {

bool isCompressed(TextureFormat::Enum) { return false; }
bool isDepth(TextureFormat::Enum) { return false; }
uint8_t getBitsPerPixel(TextureFormat::Enum) { return 32; }
const char* getName(TextureFormat::Enum) { return "unknown"; }

ImageBlockInfo s_blockInfo;
const ImageBlockInfo& getBlockInfo(TextureFormat::Enum) { return s_blockInfo; }

uint32_t imageGetSize(TextureInfo* _info, uint16_t _width, uint16_t _height, uint16_t _depth,
                       bool _cubeMap, bool _hasMips, uint16_t _numLayers, TextureFormat::Enum _format) {
    if (_info) {
        _info->width = _width; _info->height = _height; _info->depth = _depth;
        _info->numLayers = _numLayers; _info->numMips = _hasMips ? 1 : 1;
        _info->cubeMap = _cubeMap; _info->format = _format;
    }
    return _width * _height * getBitsPerPixel(_format) / 8;
}

bool imageParse(ImageContainer&, const void*, uint32_t, bx::Error*) { return false; }
void imageDecodeToBgra8(bx::AllocatorI*, void*, const void*, uint32_t, uint32_t, uint32_t, TextureFormat::Enum) {}
bool imageGetRawData(const ImageContainer&, uint16_t, uint8_t, const void*, uint32_t, ImageMip&) { return false; }
void imageSwizzleBgra8(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t) {}
int32_t imageWriteTga(bx::WriterI*, uint32_t, uint32_t, uint32_t, const void*, bool, bool, bx::Error*) { return 0; }

bool imageConvert(TextureFormat::Enum, TextureFormat::Enum) { return false; }
bool imageConvert(bx::AllocatorI*, void*, TextureFormat::Enum, const void*, TextureFormat::Enum, uint32_t, uint32_t, uint32_t) { return false; }

} // namespace bimg
