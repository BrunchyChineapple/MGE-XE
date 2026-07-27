#pragma once

#include <cstddef>
#include <cstdint>

namespace RetainedCatalog {
    constexpr std::uint32_t Magic = 0x43575452u; // "RTWC" in little-endian storage
    constexpr std::uint32_t Version = 4;

    enum class Category : std::uint32_t {
        Terrain = 1,
        Static = 2,
    };

    // Which distant-static visibility tier a placement came from. MGE-XE sorts statics
    // into three quadtrees by object size and draws each to its own range
    // (Configuration.DL.{Near,Far,VeryFar}StaticEnd), so the tier is what lets a
    // consumer retain small objects only nearby while keeping landmarks to the horizon.
    enum class Tier : std::uint32_t {
        Near = 1,
        Far = 2,
        VeryFar = 3,
    };

    enum MeshFlags : std::uint32_t {
        MeshFlagNone = 0,
        MeshFlagHasAlpha = 1u << 0,
        MeshFlagAnimatedUv = 1u << 1,
        MeshFlagIndex32 = 1u << 2,
        MeshFlagCompositeDxt1 = 1u << 3,
    };

#pragma pack(push, 4)
    struct Header {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t headerBytes;
        std::uint32_t flags;
        std::uint64_t generation;
        std::uint64_t contentHash;
        std::uint32_t cellCount;
        std::uint32_t meshCount;
        std::uint32_t placementCount;
        std::uint32_t blobBytes;
    };

    struct Cell {
        std::int32_t cellX;
        std::int32_t cellY;
        std::uint32_t terrainMeshFirst;
        std::uint32_t terrainMeshCount;
        std::uint32_t staticPlacementFirst;
        std::uint32_t staticPlacementCount;
    };
    struct Mesh {
        std::uint64_t identity;
        std::uint64_t materialIdentity;
        Category category;
        std::uint32_t flags;
        std::uint32_t vertexOffset;
        std::uint32_t vertexBytes;
        std::uint32_t vertexCount;
        std::uint32_t vertexStride;
        std::uint32_t indexOffset;
        std::uint32_t indexBytes;
        std::uint32_t indexCount;
        std::uint32_t indexStride;
        std::uint32_t materialOffset;
        std::uint32_t materialBytes;
        std::int32_t cellX;
        std::int32_t cellY;
        std::uint64_t sourceNifIdentity;
        std::uint32_t sourceStaticId;
        std::uint32_t sourceSubsetIndex;
        std::uint32_t sourceNifOffset;
        std::uint32_t sourceNifBytes;
    };

    struct Placement {
        std::uint64_t identity;
        std::uint64_t prototypeIdentity;
        std::uint64_t sourceRecordIdentity;
        std::uint32_t sourceRecordOffset;
        std::uint32_t sourceRecordBytes;
        float transform[16];
        std::int32_t cellX;
        std::int32_t cellY;
        std::uint32_t flags;
        Tier tier;
    };
#pragma pack(pop)

    static_assert(sizeof(Header) == 48);
    static_assert(offsetof(Header, contentHash) == 24);
    static_assert(offsetof(Header, blobBytes) == 44);
    static_assert(sizeof(Cell) == 24);
    static_assert(offsetof(Cell, staticPlacementFirst) == 16);
    static_assert(sizeof(Mesh) == 96);
    static_assert(offsetof(Mesh, vertexOffset) == 24);
    static_assert(offsetof(Mesh, materialOffset) == 56);
    static_assert(offsetof(Mesh, sourceNifIdentity) == 72);
    static_assert(offsetof(Mesh, sourceNifOffset) == 88);
    static_assert(sizeof(Placement) == 112);
    static_assert(offsetof(Placement, sourceRecordIdentity) == 16);
    static_assert(offsetof(Placement, transform) == 32);
    static_assert(offsetof(Placement, flags) == 104);
    static_assert(offsetof(Placement, tier) == 108);
    static_assert(sizeof(Tier) == 4);
}
