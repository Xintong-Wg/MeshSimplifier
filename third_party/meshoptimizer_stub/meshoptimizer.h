#pragma once
#include <cstddef>
#include <cstdint>

// Minimal stub of meshoptimizer for compilation
// Replace with real meshoptimizer in production

enum {
    meshopt_SimplifyLockBorder = 1 << 0,
    meshopt_SimplifySparse = 1 << 1,
    meshopt_SimplifyErrorAbsolute = 1 << 2,
    meshopt_SimplifyPrune = 1 << 3,
    meshopt_SimplifyPreserveBoundary = 1 << 4,
};

inline size_t meshopt_simplify(unsigned int* destination,
                               const unsigned int* indices,
                               size_t index_count,
                               const float* vertex_positions,
                               size_t vertex_count,
                               size_t vertex_positions_stride,
                               size_t target_index_count,
                               float target_error,
                               unsigned int options)
{
    (void)vertex_positions; (void)vertex_count; (void)vertex_positions_stride;
    (void)target_error; (void)options;

    size_t sourceTriangles = index_count / 3;
    size_t targetTriangles = target_index_count / 3;

    // Uniform sampling instead of truncation to avoid cutting off parts
    if (targetTriangles < sourceTriangles && sourceTriangles > 0) {
        float step = static_cast<float>(sourceTriangles) / static_cast<float>(targetTriangles);
        for (size_t t = 0; t < targetTriangles; ++t) {
            size_t srcTri = static_cast<size_t>(t * step);
            if (srcTri >= sourceTriangles) srcTri = sourceTriangles - 1;
            destination[t * 3 + 0] = indices[srcTri * 3 + 0];
            destination[t * 3 + 1] = indices[srcTri * 3 + 1];
            destination[t * 3 + 2] = indices[srcTri * 3 + 2];
        }
        return targetTriangles * 3;
    }

    // Keep all triangles when target is >= source
    for (size_t i = 0; i < index_count; ++i) {
        destination[i] = indices[i];
    }
    return index_count;
}

inline size_t meshopt_optimizeVertexFetchRemap(unsigned int* destination,
                                                const unsigned int* indices,
                                                size_t index_count,
                                                size_t vertex_count)
{
    for (size_t i = 0; i < vertex_count; ++i) destination[i] = static_cast<unsigned int>(i);
    (void)indices; (void)index_count;
    return vertex_count;
}

inline void meshopt_optimizeVertexCache(unsigned int* destination,
                                        const unsigned int* indices,
                                        size_t index_count,
                                        size_t vertex_count)
{
    for (size_t i = 0; i < index_count; ++i) destination[i] = indices[i];
    (void)vertex_count;
}

inline void meshopt_optimizeOverdraw(unsigned int* destination,
                                     const unsigned int* indices,
                                     size_t index_count,
                                     const float* vertex_positions,
                                     size_t vertex_count,
                                     size_t vertex_positions_stride,
                                     float threshold)
{
    for (size_t i = 0; i < index_count; ++i) destination[i] = indices[i];
    (void)vertex_positions; (void)vertex_count; (void)vertex_positions_stride; (void)threshold;
}
