#pragma once

#include "proxydx/d3d9header.h"
#include "mge/dlmath.h"
#include "ipc/retainedcatalog.h"

#include <cstddef>
#include <cstdint>

// we could use the MS extensions __ptr32 and __ptr64 instead of this conditional definition,
// but that makes the value appear as a pointer on both sides, which might give the
// impression that the pointer is valid on both sides. with the condition, we can represent
// non-shared pointers as opaque integers on the remote side, so there's no chance of confusion.
#ifdef MGE64_HOST
template<typename T> using ptr32 = std::uint32_t;
template<typename T> using ptr64 = T*;
#else
template<typename T> using ptr32 = T*;
template<typename T> using ptr64 = std::uint64_t;
#endif
// handles, on the other hand, are always opaque, so we wouldn't try to dereference one. also,
// a 32-bit handle could be valid on the 64-bit side if it's inherited. so for those reasons,
// we will use __ptr32 here.
typedef void* __ptr32 HANDLE32;

constexpr DWORD VIS_NEAR =     0x01;
constexpr DWORD VIS_FAR =      0x02;
constexpr DWORD VIS_VERY_FAR = 0x04;
constexpr DWORD VIS_GRASS =    0x08;
constexpr DWORD VIS_LAND =     0x10;
constexpr DWORD VIS_STATIC = VIS_NEAR | VIS_FAR | VIS_VERY_FAR;

// ensure consistent layout between 32-bit and 64-bit processes
#pragma pack(push, 4)
struct RenderMesh {
    bool enabled, hasAlpha, animateUV;

    ptr32<IDirect3DTexture9> tex;
    D3DXMATRIX transform;
    int verts;
    ptr32<IDirect3DVertexBuffer9> vBuffer;
    int faces;
    ptr32<IDirect3DIndexBuffer9> iBuffer;

    // NEW (additive; appended to preserve the #pragma pack(4) 32/64-bit layout).
    // Per-chunk distant-land composite (Architecture B). When compositeTex is null,
    // the Texture_Binder resolves the chunk via (cellX, cellY) through the resident
    // cache, and falls back to the global world.dds atlas when that also misses.
    // cellValid is false on Old_Format so the binder always takes the atlas path.
    ptr32<IDirect3DTexture9> compositeTex;  // null -> binder uses cell/cache or atlas
    int32_t cellX, cellY;
    bool    cellValid;
};

// One per cell newly entering the Visible_Cell_Set, streamed server -> client by the
// StreamVisibleComposites command. The compressed DXT1 bytes (byteLength, incl. mips)
// follow in the shared-memory window referenced by the IPC::Vec<CompositeChunkMsg>
// element; this header carries only the cell identity and shape. Pure fixed-width
// record so the 32-bit client and 64-bit server agree on layout under #pragma pack(4).
struct CompositeChunkMsg {
    int32_t  cellX, cellY;
    uint32_t edgeTexels;
    uint32_t byteLength;        // DXT1 + mips
};

// Delta element sent client -> server in the StreamVisibleComposites request: one cell
// newly entering the Visible_Cell_Set this frame (the client computes the set difference
// newVisible \ alreadyResident and fills a Vec of these). Pure fixed-width coordinate
// pair so the 32-bit client and 64-bit server agree on layout under #pragma pack(4). The
// server resolves each coordinate against its composite pool and streams back the matching
// CompositeChunkMsg header plus the cell's compressed bytes.
struct CompositeCellId {
    int32_t cellX, cellY;
};

struct ViewFrustum {
    D3DXPLANE frustum[6];
    enum Containment { INSIDE, OUTSIDE, INTERSECTS };

    ViewFrustum(const D3DXMATRIX* viewProj);

    Containment ContainsSphere(const BoundingSphere& sphere) const;
    Containment ContainsBox(const BoundingBox& box) const;
};

enum VisibleSetSort : std::uint8_t {
    None,
    ByState,
    ByTexture,
};

namespace IPC {
    constexpr DWORD MaxWait = 60000;

    typedef std::uint32_t VecId;
    constexpr VecId InvalidVector = static_cast<VecId>(-1);

    static inline void CleanupHandle(HANDLE& h) {
        if (h != INVALID_HANDLE_VALUE && h != NULL) {
            CloseHandle(h);
        }
        h = INVALID_HANDLE_VALUE;
    }

    // these APIs aren't supported until Windows 8 or 10, so we load them dynamically
    typedef decltype(&::MapViewOfFile3) MapViewOfFile3_t;
    typedef decltype(&::UnmapViewOfFileEx) UnmapViewOfFileEx_t;
    typedef decltype(&::VirtualAlloc2) VirtualAlloc2_t;

    extern MapViewOfFile3_t MapViewOfFile3;
    extern UnmapViewOfFileEx_t UnmapViewOfFileEx;
    extern VirtualAlloc2_t VirtualAlloc2;

    extern bool initImports();

    class Client;
    class Server;

    enum WakeReason {
        Update,
        Complete,
        ServerLost,
        Timeout,
        Error
    };

    enum Command: std::uint32_t {
        None,
        AllocVec,
        FreeVec,
        Exit,
        UpdateDynVis,
        InitDistantStatics,
        InitLandscape,
        SetWorldSpace,
        GetVisibleMeshesCoarse,
        GetVisibleMeshes,
        SortVisibleSet,
        // NEW (appended last so every prior command's value is unchanged).
        // Server fills an IPC::Vec<CompositeChunkMsg> for the delta of cells newly
        // entering the Visible_Cell_Set this frame (Composite_Streamer).
        StreamVisibleComposites,
        // Complete terrain/non-grass-static snapshot for explicit retained ownership.
        // Appended to preserve every existing command value.
        GetRetainedWorldCatalog,
    };

    struct AllocVecParameters {
        IN std::uint32_t maxCapacityInElements;
        IN std::uint32_t windowSizeInElements;
        IN std::uint32_t elementSize;
        IN std::uint32_t initialCapacity;

        OUT std::uint32_t reservedBytes;
        OUT std::uint32_t windowBytes;
        OUT std::uint32_t headerBytes;
        OUT HANDLE32 sharedMem32;
        OUT VecId id;
    };

    struct FreeVecParameters {
        IN VecId id;

        OUT bool wasFreed;
    };

    struct DynVisFlag {
        std::uint16_t groupIndex;
        bool enable;
    };

    struct DynVisParameters {
        IN VecId id;
    };

    struct DistantStaticParameters {
        IN VecId distantStatics;
        IN VecId distantSubsets;
    };

    struct LandscapeBuffers {
        ptr32<IDirect3DVertexBuffer9> vb;
        ptr32<IDirect3DIndexBuffer9> ib;
    };

    struct InitLandscapeParameters {
        IN VecId buffers;
        IN ptr32<IDirect3DTexture9> texWorldColour;   // unchanged: atlas / fallback
        // NEW (additive). Server learns whether a composite pool exists and its shape.
        // hasCompositeSet == false => Old_Format: the Composite_Streamer stays inert
        // and the client binds the atlas only (Single_Atlas_Path).
        IN bool     hasCompositeSet;
        IN uint32_t compositeCellCount;
        IN uint32_t defaultEdgeTexels;
    };

    struct SetWorldSpaceParameters {
        IN char cellname[64];

        OUT bool cellFound;
    };

    struct GetMeshesParameters {
        IN VecId visibleSet;
        IN VisibleSetSort sort;
        IN ViewFrustum viewFrustum;
        IN DWORD setFlags;
        IN D3DXVECTOR4 viewSphere;
    };

    // StreamVisibleComposites request parameters (Composite_Streamer). The client fills
    // `delta` (a Vec<CompositeCellId> of cells newly entering the Visible_Cell_Set this
    // frame), and the server appends one CompositeChunkMsg header per resolved cell to
    // `outHeaders` and that cell's compressed DXT1 + mip blob to `outBytes`, in matching
    // order. All three are shared-vector ids (the byte channel carries the variable-length
    // payload that a fixed-stride header vec cannot inline). For an Old_Format / inert pool
    // the server leaves both output channels empty (Req 4.6).
    struct StreamCompositesParameters {
        IN  VecId delta;        // Vec<CompositeCellId>: newly-visible cells
        OUT VecId outHeaders;   // Vec<CompositeChunkMsg>: one per streamed cell
        OUT VecId outBytes;     // Vec<uint8_t>: concatenated DXT1 blobs, byteLength each
    };

    struct RetainedCatalogParameters {
        OUT VecId header;       // Vec<RetainedCatalog::Header>, exactly one on success
        OUT VecId cells;        // Vec<RetainedCatalog::Cell>
        OUT VecId meshes;       // Vec<RetainedCatalog::Mesh>
        OUT VecId placements;   // Vec<RetainedCatalog::Placement>
        OUT VecId blob;         // Vec<uint8_t>, offsets in Mesh are relative to this vec
        OUT bool available;
    };

	struct Parameters {
        Command command;
        union {
            AllocVecParameters allocVecParams;
            FreeVecParameters freeVecParams;
            DynVisParameters dynVisParams;
            DistantStaticParameters distantStaticParams;
            InitLandscapeParameters initLandscapeParams;
            SetWorldSpaceParameters worldSpaceParams;
            GetMeshesParameters meshParams;
            StreamCompositesParameters streamCompositesParams;
            RetainedCatalogParameters retainedCatalogParams;
        } params;
	};

    static_assert(sizeof(RetainedCatalogParameters) == 24);
    static_assert(offsetof(RetainedCatalogParameters, available) == 20);
    static_assert(offsetof(Parameters, params) == 4);
}
#pragma pack(pop)