#pragma once

#include "Core/Types.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace mf {

// ------------------------------------------------------------------
// Eigen <-> glm conversions for OpenCASCADE interop
// ------------------------------------------------------------------
inline Eigen::Vector3d toEigen(const Vec3& v) {
    return Eigen::Vector3d(v.x, v.y, v.z);
}

inline Vec3 toGLM(const Eigen::Vector3d& v) {
    return Vec3(static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z()));
}

inline Eigen::Matrix4d toEigen(const Mat4& m) {
    Eigen::Matrix4d em;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            em(i, j) = m[i][j];
    return em;
}

inline Mat4 toGLM(const Eigen::Matrix4d& m) {
    Mat4 gm;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            gm[i][j] = static_cast<float>(m(i, j));
    return gm;
}

// ------------------------------------------------------------------
// Transform composition
// ------------------------------------------------------------------
struct Transform {
    Vec3 translation = Vec3(0.0f);
    Quat rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    Vec3 scale = Vec3(1.0f);

    Mat4 matrix() const;
    Transform inverse() const;
    bool isIdentity() const;
};

inline Mat4 Transform::matrix() const {
    Mat4 m = glm::translate(Mat4(1.0f), translation);
    m *= glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    return m;
}

inline bool Transform::isIdentity() const {
    return translation == Vec3(0.0f) && rotation == Quat(1.0f, 0.0f, 0.0f, 0.0f) && scale == Vec3(1.0f);
}

// ------------------------------------------------------------------
// Decompose a 4x4 matrix into Transform (translation, rotation, scale)
// ------------------------------------------------------------------
inline Transform mat4ToTransform(const Mat4& m) {
    Transform t;
    t.translation = Vec3(m[3]);

    Vec3 col0(m[0]);
    Vec3 col1(m[1]);
    Vec3 col2(m[2]);
    t.scale = Vec3(glm::length(col0), glm::length(col1), glm::length(col2));

    if (t.scale.x > 1e-6f) col0 /= t.scale.x;
    if (t.scale.y > 1e-6f) col1 /= t.scale.y;
    if (t.scale.z > 1e-6f) col2 /= t.scale.z;

    Mat3 rotMat(col0, col1, col2);
    t.rotation = glm::quat_cast(rotMat);

    return t;
}

} // namespace mf
