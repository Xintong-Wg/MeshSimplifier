#include "Scene/SceneGraph.h"
#include "Core/Logger.h"
#include <algorithm>
#include <stack>

namespace mf {

static EntityId s_nextId = 1;

SceneNode::SceneNode(std::string name, Type type)
    : m_name(std::move(name)), m_type(type), m_id(s_nextId++) {}

void SceneNode::addChild(std::shared_ptr<SceneNode> child) {
    if (!child) return;
    child->m_parent = weak_from_this();
    m_children.push_back(std::move(child));
}

void SceneNode::removeChild(const std::shared_ptr<SceneNode>& child) {
    auto it = std::remove(m_children.begin(), m_children.end(), child);
    m_children.erase(it, m_children.end());
}

void SceneNode::clearChildren() {
    m_children.clear();
}

void SceneNode::setLocalTransform(const Transform& t) {
    m_local = t;
    m_localMatrix = t.matrix();
    m_useLocalMatrix = false;
    markDirty();
}

void SceneNode::setLocalMatrix(const Mat4& m) {
    m_localMatrix = m;
    m_local = mat4ToTransform(m);
    m_useLocalMatrix = true;
    markDirty();
}

Mat4 SceneNode::worldTransform() const {
    if (m_dirty) {
        const Mat4 local = m_useLocalMatrix ? m_localMatrix : m_local.matrix();
        if (auto p = m_parent.lock()) {
            m_world = p->worldTransform() * local;
        } else {
            m_world = local;
        }
        m_dirty = false;
    }
    return m_world;
}

AABB SceneNode::worldAABB() const {
    Mat4 w = worldTransform();
    if (m_mesh && !m_mesh->lods.empty()) {
        const auto& aabb = m_mesh->lods[0].mesh.aabb;
        AABB waabb;
        for (int i = 0; i < 8; ++i) {
            Vec3 p(
                (i & 1) ? aabb.max.x : aabb.min.x,
                (i & 2) ? aabb.max.y : aabb.min.y,
                (i & 4) ? aabb.max.z : aabb.min.z
            );
            Vec3 wp = Vec3(w * Vec4(p, 1.0f));
            waabb.expand(wp);
        }
        m_worldAABB = waabb;
    }
    return m_worldAABB;
}

void SceneNode::setMesh(std::shared_ptr<LODMesh> mesh) {
    m_mesh = std::move(mesh);
}

void SceneNode::setPrototype(std::shared_ptr<SceneNode> proto) {
    m_prototype = proto;
}

void SceneNode::markDirty() {
    m_dirty = true;
    for (auto& c : m_children) {
        c->markDirty();
    }
}

void SceneNode::updateWorldTransforms() {
    traverse([](SceneNode* node) {
        (void)node->worldTransform();
    });
}

void SceneNode::traverse(const std::function<void(SceneNode*)>& fn) {
    fn(this);
    for (auto& c : m_children) {
        c->traverse(fn);
    }
}

void SceneNode::traverseVisible(const std::function<void(SceneNode*)>& fn, const Frustum* frustum) {
    if (frustum) {
        AABB waabb = worldAABB();
        if (!waabb.isEmpty() && !frustum->intersects(waabb)) return;
    }
    fn(this);
    for (auto& c : m_children) {
        c->traverseVisible(fn, frustum);
    }
}

bool Frustum::intersects(const AABB& aabb) const {
    for (const auto& p : planes) {
        Vec3 pmin, pmax;
        pmin.x = (p.x > 0) ? aabb.min.x : aabb.max.x;
        pmin.y = (p.y > 0) ? aabb.min.y : aabb.max.y;
        pmin.z = (p.z > 0) ? aabb.min.z : aabb.max.z;
        if (p.x * pmin.x + p.y * pmin.y + p.z * pmin.z + p.w > 0) return false;
    }
    return true;
}

Scene::Scene() {
    m_root = std::make_shared<SceneNode>("Root", SceneNode::Type::Group);
}

void Scene::detectInstances() {
    // Group nodes by mesh name / hash
    std::unordered_map<std::string, std::vector<std::shared_ptr<SceneNode>>> groups;
    m_root->traverse([&](SceneNode* node) {
        if (node->type() == SceneNode::Type::Mesh && node->mesh()) {
            groups[node->mesh()->name].push_back(node->shared_from_this());
        }
    });

    for (auto& [key, nodes] : groups) {
        if (nodes.size() < 2) continue;
        // Mark all but first as instances
        for (size_t i = 1; i < nodes.size(); ++i) {
            nodes[i]->setPrototype(nodes[0]);
        }
    }

    MF_INFO("Instance detection: {} groups with duplicates", groups.size());
}

std::vector<Scene::InstanceBatch> Scene::buildInstanceBatches() const {
    std::unordered_map<std::string, InstanceBatch> batches;

    m_root->traverse([&](SceneNode* node) {
        if (node->type() != SceneNode::Type::Mesh || !node->mesh()) return;

        auto& batch = batches[node->mesh()->name];
        if (!batch.mesh) {
            batch.mesh = node->mesh();
            batch.material = node->material();
        }
        batch.transforms.push_back(node->worldTransform());
    });

    std::vector<InstanceBatch> result;
    result.reserve(batches.size());
    for (auto& [k, v] : batches) {
        result.push_back(std::move(v));
    }
    return result;
}

void Scene::clear() {
    m_root->clearChildren();
}

} // namespace mf
