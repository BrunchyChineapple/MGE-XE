#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Canonical worldspace exterior-cell coordinate identifying one Per_Cell_Composite
// (Architecture B). This is the SINGLE definition shared by every part of the feature:
//   - the 64-bit server pool index (mge/dlstreamer.h CompositePool, ipc/dlshare.cpp)
//   - the 32-bit client resident cache (mge/compositecache.h ResidentCompositeCache)
//   - the per-frame reconcile loop (mge/distantinit.cpp, task 8.5)
// It lives here, in the lowest-level composite header both sides already include, because
// distantland.h pulls in BOTH compositecache.h and (via ipc/dlshare.h) dlstreamer.h in one
// translation unit; two separate global `struct CellId` definitions would be a C2011
// redefinition. The layout matches tests/composite_stream_model.py's CellId (x, y).
struct CellId {
    int32_t x = 0;
    int32_t y = 0;

    bool operator==(const CellId& other) const {
        return x == other.x && y == other.y;
    }
    bool operator!=(const CellId& other) const {
        return !(*this == other);
    }
};

// Hash for CellId so it can key an unordered_map / unordered_set. Mixes the two signed
// coordinates into a single 64-bit value (golden-ratio multiply on y) before hashing so
// adjacent cells don't collide trivially. A hash need only be internally consistent within
// a map, so the same implementation serves both the client and server indices.
struct CellIdHash {
    std::size_t operator()(const CellId& c) const {
        const std::uint64_t ux = static_cast<std::uint32_t>(c.x);
        const std::uint64_t uy = static_cast<std::uint32_t>(c.y);
        return std::hash<std::uint64_t>()((ux << 32) ^ (uy * 0x9E3779B97F4A7C15ull));
    }
};

// On-disk layout for the per-cell distant-land composite set (Architecture B).
//
// The composite set is written additively alongside the unchanged world / world.dds /
// world_n.dds outputs as two files under Data Files\distantland\:
//   composite.index  -> CompositeSetHeader, then cellCount CompositeDirEntry records,
//                       then the per-cell UV blob (CompositeUV pairs).
//   composite.data   -> concatenated per-cell DXT1 blobs (each includes its mip chain).
//
// The structs below are pure file records: they carry no D3D handles and no runtime
// pointers, so the same header compiles to an identical layout in the 32-bit client
// (d3d8.dll) and the 64-bit server (mgeHost64.exe). #pragma pack(4) plus fixed-width
// types keep uint64_t fields 4-byte aligned in both processes, and the trailing
// reserved bytes keep the directory entry append-only for future fields.

// Composite-set format version. Independent of MGE_DL_VERSION (the distant-statics
// marker) so the two formats can evolve without invalidating each other.
#define COMPOSITE_FORMAT_VERSION 1

// CompositeSetHeader::magic. Exactly 8 bytes, stored without a null terminator.
inline constexpr char kCompositeMagic[8] = { 'M', 'G', 'E', 'D', 'L', 'C', 'M', 'P' };

// CompositeSetHeader::flags bits.
enum CompositeSetFlags : std::uint32_t {
    COMPOSITE_FLAG_HAS_PLACEHOLDERS = 0x1,  // at least one cell baked a placeholder
};

#pragma pack(push, 4)

// Start of composite.index. magic + formatVersion form the version marker the
// Format_Loader reads to classify a set as New_Format vs Old/absent.
struct CompositeSetHeader {
    char     magic[8];            // kCompositeMagic, not null-terminated
    uint32_t formatVersion;       // COMPOSITE_FORMAT_VERSION
    uint32_t cellCount;           // number of CompositeDirEntry records that follow
    uint32_t defaultEdgeTexels;   // baked per-cell resolution (default 1024)
    uint32_t flags;               // CompositeSetFlags
};

// One per exterior distant-land cell the generator processed. Failed cells are still
// present here, flagged isPlaceholder, so the directory stays dense.
struct CompositeDirEntry {
    int32_t  cellX, cellY;        // worldspace cell coordinate
    uint32_t edgeTexels;          // this cell's baked resolution
    uint64_t dataOffset;          // byte offset into composite.data
    uint32_t dataLength;          // DXT1 + mips byte length in composite.data
    uint32_t chunkId;             // distant-land mesh chunk this cell maps to
    uint32_t uvOffset;            // byte offset into the per-cell UV blob (composite.index)
    uint32_t uvVertCount;         // number of CompositeUV pairs for this cell
    uint8_t  isPlaceholder;       // non-zero if this cell fell back to a placeholder bake
    uint8_t  reserved[3];         // pad to a 4-byte boundary; keep append-only growth room
};

// Per-cell UV blob element: one [0,1] coordinate pair per terrain vertex, packed
// contiguously and addressed by CompositeDirEntry::uvOffset / uvVertCount.
struct CompositeUV {
    float u, v;
};

#pragma pack(pop)

// Lock the on-disk layout so a 32/64-bit ABI drift fails the build instead of
// silently desyncing the two processes that read the same files.
static_assert(sizeof(CompositeSetHeader) == 24, "CompositeSetHeader layout changed");
static_assert(sizeof(CompositeDirEntry) == 40, "CompositeDirEntry layout changed");
static_assert(sizeof(CompositeUV) == 8, "CompositeUV layout changed");
static_assert(offsetof(CompositeSetHeader, formatVersion) == 8, "header version marker moved");
static_assert(offsetof(CompositeDirEntry, dataOffset) == 12, "dir entry dataOffset misaligned");
static_assert(offsetof(CompositeDirEntry, isPlaceholder) == 36, "dir entry placeholder flag moved");
