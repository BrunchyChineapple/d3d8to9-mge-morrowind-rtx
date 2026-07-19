#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#define XXH_VECTOR 0
#pragma optimize("", off)
#include "xxhash.h"
#pragma optimize("", on)

namespace LegacyGeometryHash {

struct TriangleList {
    std::uint64_t geometryHash = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::vector<std::uint32_t> indices;
};

// Computes Remix's canonical legacy geometry hash for an indexed triangle-list draw.
// Positions must be FLOAT3 values at offset zero in each vertex. The original index width
// remains part of the descriptor and index hash even though the returned indices are uint32.
inline bool computeTriangleList(
    const void* vertexData,
    std::size_t vertexBytes,
    std::uint32_t vertexStride,
    std::int32_t baseVertexIndex,
    const void* indexData,
    std::size_t indexBytes,
    std::uint32_t indexStride,
    std::uint32_t startIndex,
    std::uint32_t indexCount,
    TriangleList& output) {
    output = {};
    if (!vertexData || !indexData || vertexStride < sizeof(float) * 3 ||
        (indexStride != 2 && indexStride != 4) ||
        indexCount < 3 || indexCount % 3 != 0) {
        return false;
    }

    const std::size_t availableIndices = indexBytes / indexStride;
    if (startIndex > availableIndices ||
        indexCount > availableIndices - startIndex) {
        return false;
    }

    const auto* sourceIndices = static_cast<const std::uint8_t*>(indexData) +
        static_cast<std::size_t>(startIndex) * indexStride;
    std::vector<std::uint32_t> rawIndices(indexCount);
    std::uint32_t minIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maxIndex = 0;
    for (std::uint32_t i = 0; i < indexCount; ++i) {
        std::uint32_t index = 0;
        if (indexStride == 4) {
            std::memcpy(&index, sourceIndices + static_cast<std::size_t>(i) * 4, 4);
        } else {
            std::uint16_t index16 = 0;
            std::memcpy(&index16, sourceIndices + static_cast<std::size_t>(i) * 2, 2);
            index = index16;
        }
        rawIndices[i] = index;
        minIndex = index < minIndex ? index : minIndex;
        maxIndex = index > maxIndex ? index : maxIndex;
    }

    if (maxIndex == minIndex) {
        return false;
    }

    const std::int64_t firstVertex =
        static_cast<std::int64_t>(baseVertexIndex) + minIndex;
    const std::int64_t lastVertex =
        static_cast<std::int64_t>(baseVertexIndex) + maxIndex;
    const std::size_t availableVertices = vertexBytes / vertexStride;
    if (firstVertex < 0 || lastVertex < firstVertex ||
        static_cast<std::uint64_t>(lastVertex) >= availableVertices) {
        return false;
    }

    const std::uint64_t vertexCount64 =
        static_cast<std::uint64_t>(maxIndex) - minIndex + 1;
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    TriangleList result;
    result.firstVertex = static_cast<std::uint32_t>(firstVertex);
    result.vertexCount = static_cast<std::uint32_t>(vertexCount64);
    result.indices.resize(indexCount);
    for (std::uint32_t i = 0; i < indexCount; ++i) {
        result.indices[i] = rawIndices[i] - minIndex;
    }

    std::uint64_t indexHash = 0;
    if (indexStride == 4) {
        indexHash = XXH3_64bits(
            result.indices.data(),
            result.indices.size() * sizeof(result.indices[0]));
    } else {
        std::vector<std::uint16_t> indices16(indexCount);
        for (std::uint32_t i = 0; i < indexCount; ++i) {
            indices16[i] = static_cast<std::uint16_t>(result.indices[i]);
        }
        indexHash = XXH3_64bits(
            indices16.data(),
            indices16.size() * sizeof(indices16[0]));
    }

    std::uint32_t descriptor = indexCount;
    std::uint64_t descriptorHash = XXH3_64bits_withSeed(
        &descriptor, sizeof(descriptor), 0);
    descriptor = result.vertexCount;
    descriptorHash = XXH3_64bits_withSeed(
        &descriptor, sizeof(descriptor), descriptorHash);
    descriptor = 3; // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    descriptorHash = XXH3_64bits_withSeed(
        &descriptor, sizeof(descriptor), descriptorHash);
    descriptor = indexStride == 4 ? 1u : 0u; // VK_INDEX_TYPE_UINT32 / UINT16
    descriptorHash = XXH3_64bits_withSeed(
        &descriptor, sizeof(descriptor), descriptorHash);

    std::vector<std::uint8_t> usedVertices(result.vertexCount, 0);
    for (const auto index : result.indices) {
        usedVertices[index] = 1;
    }

    const auto* firstPosition = static_cast<const std::uint8_t*>(vertexData) +
        static_cast<std::size_t>(result.firstVertex) * vertexStride;
    std::uint64_t positionHash = 0;
    for (std::uint32_t i = 0; i < result.vertexCount; ++i) {
        if (usedVertices[i]) {
            positionHash = XXH3_64bits_withSeed(
                firstPosition + static_cast<std::size_t>(i) * vertexStride,
                sizeof(float) * 3,
                positionHash);
        }
    }

    result.geometryHash = positionHash;
    result.geometryHash = XXH64(
        &indexHash, sizeof(indexHash), result.geometryHash);
    result.geometryHash = XXH64(
        &descriptorHash, sizeof(descriptorHash), result.geometryHash);
    output = std::move(result);
    return true;
}

} // namespace LegacyGeometryHash
