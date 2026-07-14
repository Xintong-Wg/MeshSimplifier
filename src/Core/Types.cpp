#include "Core/Types.h"
#include <algorithm>
#include <limits>

namespace mf {

void AABB::expand(const Vec3& p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
}

void AABB::expand(const AABB& other) {
    if (other.isEmpty()) return;
    expand(other.min);
    expand(other.max);
}

Vec3 AABB::center() const {
    return (min + max) * 0.5f;
}

Vec3 AABB::extent() const {
    return max - min;
}

float AABB::diagonal() const {
    return glm::length(extent());
}

bool AABB::isEmpty() const {
    return min.x > max.x;
}

} // namespace mf
