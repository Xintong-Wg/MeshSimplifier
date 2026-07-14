#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// OpenCASCADE
#include <opencascade/TopoDS_Shape.hxx>

namespace mf {

struct AssemblyNode;
using AssemblyNodePtr = std::shared_ptr<AssemblyNode>;

// ------------------------------------------------------------------
// Assembly tree node from STEP
// ------------------------------------------------------------------
struct AssemblyNode {
    enum class Type { Root, Assembly, Part };

    std::string id;
    std::string name;
    std::string stepId;
    Type type = Type::Part;
    Mat4 localTransform = Mat4(1.0f);

    std::vector<AssemblyNodePtr> children;
    AssemblyNodePtr parent;

    std::string shapeKey;
    bool isInstance = false;
    std::string prototypeId;
};

// ------------------------------------------------------------------
// STEP parsing result
// ------------------------------------------------------------------
struct STEPResult {
    AssemblyNodePtr root;
    std::unordered_map<std::string, AssemblyNodePtr> partsById;
    std::unordered_map<std::string, std::vector<AssemblyNodePtr>> instances;
    std::unordered_map<std::string, TopoDS_Shape> shapesByKey;
    size_t entityCount = 0;
    double fileScale = 1.0;
};

// ------------------------------------------------------------------
// STEP reader with BRep binary cache
// ------------------------------------------------------------------
class STEPReader {
public:
    STEPReader();
    ~STEPReader();

    // Parse a STEP file (uses cache if available)
    std::shared_ptr<STEPResult> read(const std::string& filepath,
                                     const std::string& cacheDir = "");

    bool probe(const std::string& filepath, size_t& outEntityCount);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mf
