#pragma once

#include "Core/Types.h"
#include "Mesh/MeshData.h"
#include "Scene/SceneGraph.h"
#include <string>
#include <memory>
#include <vector>

namespace mf {

// ------------------------------------------------------------------
// glTF export options
// ------------------------------------------------------------------
struct glTFExportOptions {
    bool useDraco = true;
    int dracoPositionBits = 14;
    int dracoNormalBits = 10;
    int dracoTexCoordBits = 12;
    int dracoCompressionLevel = 7;     // 0-10, higher = smaller
    bool embedBuffers = true;          // glb
    bool exportLODs = false;           // only export highest LOD
    bool exportInstances = true;       // use EXT_mesh_gpu_instancing
    bool preserveHierarchy = true;     // export assembly tree as glTF node hierarchy
};

// ------------------------------------------------------------------
// glTF/glb exporter
// ------------------------------------------------------------------
class glTFExporter {
public:
    glTFExporter();
    ~glTFExporter();

    bool exportScene(const Scene& scene, const std::string& filepath, const glTFExportOptions& options);

    // Export only the selected nodes (and their children if they are assemblies)
    bool exportNodes(const std::vector<SceneNode*>& nodes, const std::string& filepath, const glTFExportOptions& options);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mf
