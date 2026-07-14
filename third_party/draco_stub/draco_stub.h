#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Minimal draco stub - allows compilation without real Draco

namespace draco {

using PointIndex = uint32_t;
using AttributeValueIndex = uint32_t;

enum GeometryAttributeType { INVALID = -1, POSITION = 0, NORMAL = 1, TEX_COORD = 2 };
enum DataType { DT_FLOAT32 = 1 };

struct Status {
    bool ok() const { return code == 0; }
    std::string error_msg_string() const { return "stub"; }
    int code = 0;
    static Status OK() { return Status{}; }
};

class GeometryAttribute {
public:
    using Type = GeometryAttributeType;
    static constexpr Type POSITION = draco::POSITION;
    static constexpr Type NORMAL = draco::NORMAL;
    static constexpr Type TEX_COORD = draco::TEX_COORD;

    void Init(Type, void*, int, DataType, bool, int, int) {}
};

class PointAttribute : public GeometryAttribute {
public:
    void SetAttributeValue(AttributeValueIndex, const void*) {}
    void SetAttributeValue(AttributeValueIndex, PointIndex, const void*) {}
};

class PointCloud {
public:
    using GeometryAttribute = draco::GeometryAttribute;

    void set_num_points(uint32_t) {}
    int AddAttribute(GeometryAttribute&, bool, uint32_t) { return 0; }
    PointAttribute* attribute(int) { static PointAttribute a; return &a; }
    void SetAttributeValue(int, PointIndex, const void*) {}
};

class Mesh : public PointCloud {
public:
    struct Face {
        PointIndex& operator[](int i) { static PointIndex d; return d; }
    };

    void AddFace(const Face&) {}
};

class EncoderBuffer {
public:
    size_t size() const { return m_data.size(); }
    const uint8_t* data() const { return m_data.data(); }
    std::vector<uint8_t> m_data;
};

class Encoder {
public:
    void SetSpeedOptions(int, int) {}
    void SetAttributeQuantization(GeometryAttribute::Type, int) {}
    Status EncodeMeshToBuffer(Mesh&, EncoderBuffer* buf) {
        buf->m_data = {0x01, 0x02, 0x03};
        return Status::OK();
    }
};

} // namespace draco
