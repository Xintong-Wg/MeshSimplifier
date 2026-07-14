#pragma once

#include "Core/Types.h"
#include "Math/MathUtils.h"
#include "Mesh/MeshData.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace mf {

// Forward declarations
class Renderable;
struct Frustum;

// ------------------------------------------------------------------
// Scene graph node
// ------------------------------------------------------------------
class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    enum class Type { Group, Mesh, Instance, Camera, Light };

    SceneNode(std::string name, Type type);

    void addChild(std::shared_ptr<SceneNode> child);
    void removeChild(const std::shared_ptr<SceneNode>& child);
    void clearChildren();

    const std::string& name() const { return m_name; }
    Type type() const { return m_type; }
    EntityId id() const { return m_id; }

    const Transform& localTransform() const { return m_local; }
    void setLocalTransform(const Transform& t);
    void setLocalMatrix(const Mat4& m);

    Mat4 worldTransform() const;
    AABB worldAABB() const;

    void setMesh(std::shared_ptr<LODMesh> mesh);
    std::shared_ptr<LODMesh> mesh() const { return m_mesh; }

    void setMaterial(MaterialHandle mat) { m_material = mat; }
    MaterialHandle material() const { return m_material; }

    // For instances: the prototype mesh
    void setPrototype(std::shared_ptr<SceneNode> proto);
    std::shared_ptr<SceneNode> prototype() const { return m_prototype.lock(); }

    const std::vector<std::shared_ptr<SceneNode>>& children() const { return m_children; }
    std::shared_ptr<SceneNode> parent() const { return m_parent.lock(); }

    void markDirty();
    void updateWorldTransforms();

    // Traversal
    void traverse(const std::function<void(SceneNode*)>& fn);
    void traverseVisible(const std::function<void(SceneNode*)>& fn, const Frustum* frustum = nullptr);

private:
    std::string m_name;
    Type m_type;
    EntityId m_id;
    Transform m_local;
    Mat4 m_localMatrix{1.0f};
    bool m_useLocalMatrix = false;
    mutable Mat4 m_world{1.0f};
    mutable AABB m_worldAABB;
    mutable bool m_dirty = true;

    std::vector<std::shared_ptr<SceneNode>> m_children;
    std::weak_ptr<SceneNode> m_parent;

    std::shared_ptr<LODMesh> m_mesh;
    MaterialHandle m_material;
    std::weak_ptr<SceneNode> m_prototype;
};

// ------------------------------------------------------------------
// Simple frustum for culling
// ------------------------------------------------------------------
struct Frustum {
    // 6 planes: left, right, top, bottom, near, far
    std::array<Vec4, 6> planes;
    bool intersects(const AABB& aabb) const;
};

// ------------------------------------------------------------------
// Scene container
// ------------------------------------------------------------------
class Scene {
public:
    Scene();

    std::shared_ptr<SceneNode> root() { return m_root; }
    std::shared_ptr<const SceneNode> root() const { return m_root; }

    // Instance detection: group identical meshes
    void detectInstances();

    // Build GPU instancing batches
    struct InstanceBatch {
        std::shared_ptr<LODMesh> mesh;
        std::vector<Mat4> transforms;
        MaterialHandle material;
    };
    std::vector<InstanceBatch> buildInstanceBatches() const;

    void clear();

private:
    std::shared_ptr<SceneNode> m_root;
};

} // namespace mf
