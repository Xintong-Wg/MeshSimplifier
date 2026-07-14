#pragma once
// Minimal stub of Fast Quadric Mesh Simplification for compilation
// Replace with real implementation: https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification
#include <vector>
#include <cstdint>
#include <cstring>

struct vec3f { float x, y, z; };
struct vec3ui { uint32_t x, y, z; };

namespace Simplify {
    inline void simplify_mesh(std::vector<vec3f>& vertices,
                              std::vector<vec3ui>& triangles,
                              size_t targetCount,
                              float targetError,
                              int maxIterations,
                              bool preserveBoundary) {
        (void)targetError; (void)maxIterations; (void)preserveBoundary;

        size_t sourceCount = triangles.size();
        if (targetCount >= sourceCount || sourceCount == 0) return;

        // Uniform sampling instead of truncation to avoid cutting off parts
        float step = static_cast<float>(sourceCount) / static_cast<float>(targetCount);
        std::vector<vec3ui> sampled;
        sampled.reserve(targetCount);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            if (idx >= sourceCount) idx = sourceCount - 1;
            sampled.push_back(triangles[idx]);
        }
        triangles = std::move(sampled);

        // Note: in a real implementation, vertices would also be reduced.
        // The stub keeps all vertices (they will be optimized by meshopt later).
    }
}
