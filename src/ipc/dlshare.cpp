#include "dlshare.h"

#include "mge/configuration.h"
#include "mge/dlcomposite.h"
#include "mge/distantshader.h"
#include "mge/mgeversion.h"
#include "mge/quadtree.h"
#include "support/log.h"

#include <cmath>  // std::floor for per-cell identity from mesh centre

using std::string;
using std::unordered_map;
using std::vector;

unordered_map<std::string, DistantLandShare::WorldSpace> DistantLandShare::mapWorldSpaces;
const DistantLandShare::WorldSpace* DistantLandShare::currentWorldSpace = nullptr;
bool DistantLandShare::hasCurrentWorldSpace = false;
QuadTree DistantLandShare::LandQuadTree;
vector<vector<QuadTreeMesh*>> DistantLandShare::dynamicVisGroupsServer;
CompositePool DistantLandShare::compositePool;

void DistantLandShare::loadVisGroupsServer(HANDLE h) {
    DWORD unused;

    // skip distant statics count
    SetFilePointer(h, 4, NULL, FILE_CURRENT);

    // Load dynamic vis groups
    DWORD dynamicVisGroupCount;
    ReadFile(h, &dynamicVisGroupCount, 4, &unused, 0);
    dynamicVisGroupsServer.clear();

    if (dynamicVisGroupCount > 0) {
        const DWORD visGroupRecordSize = 130;
        DWORD visDataSize = visGroupRecordSize * dynamicVisGroupCount;
        SetFilePointer(h, visDataSize, NULL, FILE_CURRENT);

        // VisGroup indexes use a 1-based index, group 0 is reserved for testing
        dynamicVisGroupsServer.resize(dynamicVisGroupCount + 1);
    }
}

HANDLE DistantLandShare::beginReadStatics() {
    DWORD unused;
    HANDLE h;

    h = CreateFile("Data Files\\distantland\\version", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        LOG::logline("!! Required distant statics files are missing, regeneration required - distantland/version");
        LOG::flush();
        return INVALID_HANDLE_VALUE;
    }
    BYTE version = 0;
    ReadFile(h, &version, sizeof(version), &unused, 0);
    if (version != MGE_DL_VERSION) {
        LOG::logline("!! Distant statics data is from an old version and needs to be regenerated");
        LOG::flush();
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle(h);

    h = CreateFile("Data Files\\distantland\\statics\\usage.data", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        LOG::logline("!! Required distant statics files are missing, regeneration required - distantland/statics/usage.data");
        LOG::flush();
        return INVALID_HANDLE_VALUE;
    }

    return h;
}

bool DistantLandShare::initDistantStaticsServer(IPC::Vec<DistantStatic>& distantStatics, IPC::Vec<DistantSubset>& distantSubsets) {
    if (GetFileAttributes("Data Files\\distantland\\statics") == INVALID_FILE_ATTRIBUTES) {
        LOG::logline("!! Distant statics have not been generated");
        LOG::flush();
        return !(Configuration.MGEFlags & USE_DISTANT_LAND);
    }

    auto h = beginReadStatics();
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    loadVisGroupsServer(h);
    readDistantStatics(h, distantStatics, distantSubsets, dynamicVisGroupsServer);

    CloseHandle(h);

    return true;
}

namespace {
    // Read an entire file into a byte buffer. Returns false (and leaves out empty) for a
    // missing file or any read error -- a missing composite.index is the Old_Format signal,
    // not an error, so the caller treats false as "fall back to single-atlas". Never throws.
    bool readWholeFile(const char* path, std::vector<uint8_t>& out) {
        out.clear();

        HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
            size.QuadPart > static_cast<LONGLONG>(0xFFFFFFFFu)) {
            // Reject absurd / unreadable sizes rather than attempting a huge allocation.
            CloseHandle(h);
            return false;
        }

        const DWORD bytes = static_cast<DWORD>(size.QuadPart);
        if (bytes == 0) {
            CloseHandle(h);
            return true;  // empty but readable; classify() will reject it as too short
        }

        out.resize(bytes);
        DWORD readTotal = 0;
        while (readTotal < bytes) {
            DWORD got = 0;
            if (!ReadFile(h, out.data() + readTotal, bytes - readTotal, &got, 0) || got == 0) {
                CloseHandle(h);
                out.clear();
                return false;
            }
            readTotal += got;
        }

        CloseHandle(h);
        return true;
    }
}

bool DistantLandShare::loadCompositeSet() {
    // Start from the inactive single-atlas-only state. Every early return below leaves the
    // pool inactive, which is exactly the Old/absent/malformed fallback signal the streamer
    // and renderer read (Requirements 3.3, 3.5, 4.6, 6.3). This function never throws.
    compositePool.clear();

    std::vector<uint8_t> index, data;

    // An absent composite.index IS the Old_Format signal -- silent, no error logged.
    if (!readWholeFile("Data Files\\distantland\\composite.index", index)) {
        return false;
    }
    // composite.data may legitimately be absent/empty; treat that as Old/absent below.
    readWholeFile("Data Files\\distantland\\composite.data", data);

    // --- Version-marker + structural validation gate ---------------------------------
    // This mirrors the authoritative model in tests/composite_format_model.py
    // (_validated_directory / classify): magic, supported version, non-zero cellCount,
    // the directory + per-cell UV spans fitting inside composite.index, and every entry's
    // data span fitting inside composite.data. Any failure => log once, stay single-atlas.
    auto rejectMalformed = [](const char* why) -> bool {
        // composite.index existed but failed validation: a genuinely malformed/old set.
        // Log ONCE here (single load-time line) and fall back; do not throw (Req 4.6, 6.3).
        LOG::logline("-- Distant landscape composites present but unusable (%s); using single atlas", why);
        DistantLandShare::compositePool.clear();
        return false;
    };

    if (index.size() < sizeof(CompositeSetHeader)) {
        return rejectMalformed("index shorter than header");
    }

    CompositeSetHeader header;
    memcpy(&header, index.data(), sizeof(header));

    if (memcmp(header.magic, kCompositeMagic, sizeof(header.magic)) != 0) {
        return rejectMalformed("bad magic");
    }
    if (header.formatVersion != COMPOSITE_FORMAT_VERSION) {
        return rejectMalformed("unknown format version");
    }
    if (header.cellCount == 0) {
        return rejectMalformed("zero cell count");
    }

    // Directory must fit inside the index buffer. Use 64-bit math so a huge cellCount
    // can't wrap a 32-bit multiply and slip past the bounds check.
    const uint64_t directoryEnd =
        static_cast<uint64_t>(sizeof(CompositeSetHeader)) +
        static_cast<uint64_t>(header.cellCount) * sizeof(CompositeDirEntry);
    if (directoryEnd > index.size()) {
        return rejectMalformed("directory exceeds index length");
    }

    const uint64_t uvBlobStart = directoryEnd;
    const uint64_t dataLen = data.size();

    // First pass: validate every directory entry's data + UV spans before mutating the
    // pool, so a malformed entry leaves the pool fully inactive (no partial state).
    for (uint32_t i = 0; i < header.cellCount; ++i) {
        CompositeDirEntry entry;
        memcpy(&entry, index.data() + sizeof(CompositeSetHeader) + i * sizeof(CompositeDirEntry), sizeof(entry));

        // composite.data must hold this cell's full DXT1 span (Req 3.5: data shorter than
        // the directory requires => fall back).
        if (entry.dataOffset + static_cast<uint64_t>(entry.dataLength) > dataLen) {
            return rejectMalformed("data span exceeds composite.data length");
        }

        // The per-cell UV pairs must lie inside the index's UV blob region.
        const uint64_t uvEnd =
            uvBlobStart + entry.uvOffset + static_cast<uint64_t>(entry.uvVertCount) * sizeof(CompositeUV);
        if (uvEnd > index.size()) {
            return rejectMalformed("UV span exceeds index length");
        }
    }

    // --- Commit: load the full compressed pool into the 64-bit address space ----------
    // The set is structurally complete (classifies New_Format). Take ownership of the
    // composite.data bytes and build the CellId -> CompositeRecord and chunkIndex -> CellId
    // indices (Requirement 4.1).
    compositePool.blob = std::move(data);
    compositePool.cellCount = header.cellCount;
    compositePool.defaultEdgeTexels = header.defaultEdgeTexels;
    compositePool.flags = header.flags;
    compositePool.byCell.reserve(header.cellCount);
    compositePool.byChunk.reserve(header.cellCount);

    for (uint32_t i = 0; i < header.cellCount; ++i) {
        CompositeDirEntry entry;
        memcpy(&entry, index.data() + sizeof(CompositeSetHeader) + i * sizeof(CompositeDirEntry), sizeof(entry));

        CellId cell{ entry.cellX, entry.cellY };

        CompositeRecord record;
        record.cell = cell;
        record.edgeTexels = entry.edgeTexels;
        record.byteLength = entry.dataLength;
        record.poolOffset = entry.dataOffset;
        record.isPlaceholder = (entry.isPlaceholder != 0);

        compositePool.byCell[cell] = record;
        compositePool.byChunk[entry.chunkId] = cell;
    }

    compositePool.active = true;

    LOG::logline("-- Distant landscape composites loaded: %u cells, %u px default, pool %u MB%s",
                 compositePool.cellCount,
                 compositePool.defaultEdgeTexels,
                 static_cast<unsigned>(compositePool.blob.size() / (1 << 20)),
                 (compositePool.flags & COMPOSITE_FLAG_HAS_PLACEHOLDERS) ? " (contains placeholders)" : "");
    return true;
}

bool DistantLandShare::initLandscapeServer(IPC::Vec<IPC::LandscapeBuffers>& landscapeBuffers, ptr32<IDirect3DTexture9> texWorldColour) {
    HANDLE file = CreateFile("Data Files\\distantland\\world", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        LOG::winerror("Server failed to open landscape data");
        return false;
    }

    DWORD file_size = GetFileSize(file, NULL);
    DWORD mesh_count, unused;
    ReadFile(file, &mesh_count, 4, &unused, 0);

    vector<LandMesh> meshesLand;
    meshesLand.resize(mesh_count);

    landscapeBuffers.start_read();
    auto it = landscapeBuffers.begin();
    if (!meshesLand.empty()) {
        D3DXVECTOR2 qtmin(FLT_MAX, FLT_MAX), qtmax(-FLT_MAX, -FLT_MAX);
        D3DXMATRIX world;
        D3DXMatrixIdentity(&world);

        // Load meshes and calculate max size of quadtree
        for (auto& i : meshesLand) {
            ReadFile(file, &i.sphere.radius, 4, &unused, 0);
            ReadFile(file, &i.sphere.center, 12, &unused, 0);

            D3DXVECTOR3 boxMin, boxMax;
            ReadFile(file, &boxMin, 12, &unused, 0);
            ReadFile(file, &boxMax, 12, &unused, 0);
            i.box.Set(boxMin, boxMax);

            ReadFile(file, &i.verts, 4, &unused, 0);
            ReadFile(file, &i.faces, 4, &unused, 0);

            bool large = (i.verts > 0xFFFF || i.faces > 0xFFFF);

            // skip info the client handles
            auto bufferSize = i.verts * SIZEOFLANDVERT + i.faces * (large ? 12 : 6);
            SetFilePointer(file, bufferSize, NULL, FILE_CURRENT);

            auto& buffers = *it;
            if (it.at_end()) {
                LOG::logline("Client landscape buffers ended while the server still has more meshes (%u buffers found, expected %u)", landscapeBuffers.size(), mesh_count);
                landscapeBuffers.end_read();
                return false;
            }
            ++it;

            i.vbuffer = buffers.vb;
            i.ibuffer = buffers.ib;

            qtmin.x = std::min(qtmin.x, i.sphere.center.x - i.sphere.radius);
            qtmin.y = std::min(qtmin.y, i.sphere.center.y - i.sphere.radius);
            qtmax.x = std::max(qtmax.x, i.sphere.center.x + i.sphere.radius);
            qtmax.y = std::max(qtmax.y, i.sphere.center.y + i.sphere.radius);
        }

        LandQuadTree.SetBox(std::max(qtmax.x - qtmin.x, qtmax.y - qtmin.y), 0.5 * (qtmax + qtmin));

        // Add meshes to the quadtree. Each mesh is now exactly one Morrowind exterior cell (the
        // tessellator emits one mesh per 8192-unit patch), so derive the cell identity from the mesh
        // centre and stamp it onto the QuadTreeMesh. The per-cell composite Texture_Binder reads
        // cellX/cellY/cellValid off the streamed RenderMesh to look the cell up in the resident cache;
        // floor(center / 8192) is the same cell mapping the streamer and generator use.
        for (auto& i : meshesLand) {
            QuadTreeMesh* qm = LandQuadTree.AddMesh(i.sphere, i.box, world, false, false, texWorldColour, i.verts, i.vbuffer, i.faces, i.ibuffer);
            if (qm) {
                qm->cellX = (int32_t)std::floor(i.sphere.center.x / 8192.0f);
                qm->cellY = (int32_t)std::floor(i.sphere.center.y / 8192.0f);
                qm->cellValid = true;
                qm->compositeTex = 0;  // resolved per frame via the cache; never a baked handle here (ptr32, so 0 not nullptr)
            }
        }
    }

    landscapeBuffers.end_read();

    CloseHandle(file);
    LandQuadTree.CalcVolume();

    // Log approximate memory use
    LOG::logline("-- Distant landscape memory use: %d MB", file_size / (1 << 20));

    // Architecture B Format_Loader (task 6.1): after the existing mesh table read, attempt
    // to load the additive per-cell composite set. loadCompositeSet() classifies the
    // on-disk composite.{index,data} as New_Format vs Old/absent/malformed, and on New
    // loads the full compressed pool into this 64-bit process and builds the
    // CellId/chunk indices (Req 3.2, 4.1). It never throws and returns false for the
    // Old/absent/malformed path, where the pool stays inactive so the Composite_Streamer
    // is inert and the renderer uses the Single_Atlas_Path (Req 3.3, 4.6, 6.3). The
    // distant-land load itself succeeds regardless of the composite outcome.
    loadCompositeSet();

    return true;
}

bool DistantLandShare::setCurrentWorldSpace(const char* name) {
    auto it = mapWorldSpaces.find(name);
    if (it != mapWorldSpaces.end()) {
        currentWorldSpace = &it->second;
        hasCurrentWorldSpace = true;
        return true;
    }

    currentWorldSpace = nullptr;
    hasCurrentWorldSpace = false;
    return false;
}

void DistantLandShare::getVisibleMeshesCoarse(IPC::Vec<RenderMesh>& output, const ViewFrustum& viewFrustum, VisibleSetSort sort, DWORD setFlags) {
    VisibleSet<IpcServerVector> visibleSet((IpcServerVector(output))); // extra parens for vexing parse

    // if we're not sorting, we can do parallel reads and writes where the client processes elements as we add them
    if (sort == VisibleSetSort::None) {
        visibleSet.StartWrite();
    }

    if (setFlags & VIS_NEAR) {
        currentWorldSpace->NearStatics->GetVisibleMeshesCoarse(viewFrustum, visibleSet);
    }
    if (setFlags & VIS_FAR) {
        currentWorldSpace->FarStatics->GetVisibleMeshesCoarse(viewFrustum, visibleSet);
    }
    if (setFlags & VIS_VERY_FAR) {
        currentWorldSpace->VeryFarStatics->GetVisibleMeshesCoarse(viewFrustum, visibleSet);
    }
    if (setFlags & VIS_GRASS) {
        currentWorldSpace->GrassStatics->GetVisibleMeshesCoarse(viewFrustum, visibleSet);
    }
    if (setFlags & VIS_LAND) {
        LandQuadTree.GetVisibleMeshesCoarse(viewFrustum, visibleSet);
    }

    switch (sort) {
    case VisibleSetSort::ByState:
        visibleSet.SortByState();
        break;
    case VisibleSetSort::ByTexture:
        visibleSet.SortByTexture();
        break;
    case VisibleSetSort::None:
        visibleSet.EndWrite();
        break;
    }
}

void DistantLandShare::getVisibleMeshes(IPC::Vec<RenderMesh>& output, const ViewFrustum& viewFrustum, const D3DXVECTOR4& viewSphere, VisibleSetSort sort, DWORD setFlags) {
    VisibleSet<IpcServerVector> visibleSet((IpcServerVector(output))); // extra parens for vexing parse

    // if we're not sorting, we can do parallel reads and writes where the client processes elements as we add them
    if (sort == VisibleSetSort::None) {
        visibleSet.StartWrite();
    }

    if (setFlags & VIS_NEAR) {
        currentWorldSpace->NearStatics->GetVisibleMeshes(viewFrustum, viewSphere, visibleSet);
    }
    if (setFlags & VIS_FAR) {
        currentWorldSpace->FarStatics->GetVisibleMeshes(viewFrustum, viewSphere, visibleSet);
    }
    if (setFlags & VIS_VERY_FAR) {
        currentWorldSpace->VeryFarStatics->GetVisibleMeshes(viewFrustum, viewSphere, visibleSet);
    }
    if (setFlags & VIS_GRASS) {
        currentWorldSpace->GrassStatics->GetVisibleMeshes(viewFrustum, viewSphere, visibleSet);
    }
    if (setFlags & VIS_LAND) {
        LandQuadTree.GetVisibleMeshes(viewFrustum, viewSphere, visibleSet);
    }

    switch (sort) {
    case VisibleSetSort::ByState:
        visibleSet.SortByState();
        break;
    case VisibleSetSort::ByTexture:
        visibleSet.SortByTexture();
        break;
    case VisibleSetSort::None:
        visibleSet.EndWrite();
        break;
    }
}

void DistantLandShare::sortVisibleSet(IPC::Vec<RenderMesh>& vec, VisibleSetSort sort) {
    VisibleSet<IpcServerVector> visibleSet((IpcServerVector(vec))); // extra parens for vexing parse

    switch (sort) {
    case VisibleSetSort::ByState:
        visibleSet.SortByState();
        break;
    case VisibleSetSort::ByTexture:
        visibleSet.SortByTexture();
        break;
    case VisibleSetSort::None:
        break;
    }
}