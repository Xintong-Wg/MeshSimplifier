#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include "Scene/SceneGraph.h"
#include <string>

namespace mf {

// ------------------------------------------------------------------
// Binary STL exporter
// ------------------------------------------------------------------
class STLExporter {
public:
    // Export entire scene as a single merged binary STL
    bool exportScene(const Scene& scene, const std::string& filepath);

    // Export only selected nodes as a single merged binary STL
    bool exportNodes(const std::vector<SceneNode*>& nodes, const std::string& filepath);

    // Export a single mesh directly
    bool exportMesh(const MeshData& mesh, const std::string& filepath, const std::string& name);
};

} // namespace mf
