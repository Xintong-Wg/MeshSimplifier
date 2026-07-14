#pragma once

#include "Core/Types.h"
#include "Geometry/BRepAnalyzer.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

class TopoDS_Shape;

namespace mf {

// ------------------------------------------------------------------
// Defeaturing parameters
// ------------------------------------------------------------------
struct DefeatureParams {
    float minHoleRadius = 2.0f;          // mm, holes smaller than this → fill
    float minFilletRadius = 1.0f;        // mm, fillets smaller than this → remove
    float minChamferWidth = 1.0f;        // mm, chamfers smaller than this → remove
    float minFeatureArea = 5.0f;         // mm², faces smaller than this → candidate
    float minProtrusionHeight = 1.0f;    // mm, protrusions lower than this → remove
    bool removeInternalFeatures = true;
};

// ------------------------------------------------------------------
struct DefeatureResult {
    std::vector<std::string> removedFaces;   // shapeKey:faceIndex
    std::vector<std::string> filledHoles;
    size_t featuresRemoved = 0;
    float areaReduction = 0.0f;

    // Modified shapes (null if unchanged)
    std::unordered_map<std::string, TopoDS_Shape> modifiedShapes;
};

// ------------------------------------------------------------------
// Automatic defeaturing processor
// ------------------------------------------------------------------
class DefeatureProcessor {
public:
    DefeatureProcessor();
    ~DefeatureProcessor();

    // Process a shape, removing small features
    DefeatureResult process(const std::unordered_map<std::string, TopoDS_Shape>& shapesByKey,
                             const std::unordered_map<std::string, ShapeAnalysis>& analyses,
                             const DefeatureParams& params = {});

    // Process a single shape
    DefeatureResult processShape(const TopoDS_Shape& shape, const ShapeAnalysis& analysis,
                                  const DefeatureParams& params);

    void setParams(const DefeatureParams& p) { m_params = p; }

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    DefeatureParams m_params;
};

} // namespace mf
