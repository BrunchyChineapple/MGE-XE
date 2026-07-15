#include "dlshare.h"

#include "mge/configuration.h"
#include "mge/dlcomposite.h"
#include "mge/distantshader.h"
#include "mge/mgeversion.h"
#include "mge/quadtree.h"
#include "support/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>

using std::string;
using std::unordered_map;
using std::vector;

unordered_map<std::string, DistantLandShare::WorldSpace> DistantLandShare::mapWorldSpaces;
const DistantLandShare::WorldSpace* DistantLandShare::currentWorldSpace = nullptr;
bool DistantLandShare::hasCurrentWorldSpace = false;
QuadTree DistantLandShare::LandQuadTree;
vector<vector<QuadTreeMesh*>> DistantLandShare::dynamicVisGroupsServer;
CompositePool DistantLandShare::compositePool;
vector<RetainedCatalog::Mesh> DistantLandShare::retainedStaticMeshes;
vector<RetainedCatalog::Mesh> DistantLandShare::retainedTerrainMeshes;
vector<std::uint8_t> DistantLandShare::retainedStaticBlob;
vector<std::uint8_t> DistantLandShare::retainedTerrainBlob;
vector<std::uint64_t> DistantLandShare::retainedStaticPrototypeIds;
std::uint64_t DistantLandShare::retainedCatalogGeneration = 0;

void DistantLandShare::bumpRetainedCatalogGeneration() noexcept {
    ++retainedCatalogGeneration;
}

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

    DWORD staticCount = 0;
    DWORD unused = 0;
    if (!ReadFile(h, &staticCount, sizeof(staticCount), &unused, nullptr) ||
        unused != sizeof(staticCount)) {
        CloseHandle(h);
        return false;
    }
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);

    if (!loadRetainedStaticCatalog(staticCount)) {
        LOG::logline("-- Retained static catalog unavailable; legacy distant statics remain active");
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

    class ByteCursor {
        const std::uint8_t* m_data;
        std::size_t m_size;
        std::size_t m_offset = 0;

    public:
        explicit ByteCursor(const std::vector<std::uint8_t>& bytes)
            : m_data(bytes.data()), m_size(bytes.size()) { }

        bool read(void* destination, std::size_t bytes) {
            if (bytes > m_size - m_offset) {
                return false;
            }
            std::memcpy(destination, m_data + m_offset, bytes);
            m_offset += bytes;
            return true;
        }

        const std::uint8_t* take(std::size_t bytes) {
            if (bytes > m_size - m_offset) {
                return nullptr;
            }
            const auto* result = m_data + m_offset;
            m_offset += bytes;
            return result;
        }

        bool atEnd() const { return m_offset == m_size; }
    };

    std::uint64_t hashBytes(
        const void* data,
        std::size_t bytes,
        std::uint64_t hash = 1469598103934665603ull) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool checkedBytes(std::uint32_t count, std::uint32_t stride, std::uint32_t& out) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) * stride;
        if (bytes > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out = static_cast<std::uint32_t>(bytes);
        return true;
    }

    bool appendBytes(
        std::vector<std::uint8_t>& destination,
        const std::uint8_t* source,
        std::uint32_t bytes,
        std::uint32_t& offset) {
        if ((!source && bytes != 0) ||
            destination.size() + static_cast<std::uint64_t>(bytes) >
                std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        offset = static_cast<std::uint32_t>(destination.size());
        if (bytes != 0) {
            destination.insert(destination.end(), source, source + bytes);
        }
        return true;
    }

    bool publishRetainedCatalog(
        const RetainedCatalog::Header& header,
        const std::vector<RetainedCatalog::Cell>& cells,
        const std::vector<RetainedCatalog::Mesh>& meshes,
        const std::vector<RetainedCatalog::Placement>& placements,
        const std::vector<std::uint8_t>& blob,
        IPC::Vec<RetainedCatalog::Header>& outHeader,
        IPC::Vec<RetainedCatalog::Cell>& outCells,
        IPC::Vec<RetainedCatalog::Mesh>& outMeshes,
        IPC::Vec<RetainedCatalog::Placement>& outPlacements,
        IPC::Vec<std::uint8_t>& outBlob) {
        const auto fail = [&]() {
            outHeader.truncate(0);
            outCells.truncate(0);
            outMeshes.truncate(0);
            outPlacements.truncate(0);
            outBlob.truncate(0);
            return false;
        };

        if (!outHeader.reserve(1) ||
            !outCells.reserve(header.cellCount) ||
            !outMeshes.reserve(header.meshCount) ||
            !outPlacements.reserve(header.placementCount) ||
            !outBlob.reserve(header.blobBytes) ||
            !outHeader.push_back(header)) {
            return fail();
        }
        for (const auto& cell : cells) {
            if (!outCells.push_back(cell)) return fail();
        }
        for (const auto& mesh : meshes) {
            if (!outMeshes.push_back(mesh)) return fail();
        }
        for (const auto& placement : placements) {
            if (!outPlacements.push_back(placement)) return fail();
        }
        for (const auto byte : blob) {
            if (!outBlob.push_back(byte)) return fail();
        }
        return true;
    }
}

bool DistantLandShare::loadRetainedStaticCatalog(std::uint32_t staticCount) {
    retainedStaticMeshes.clear();
    retainedStaticBlob.clear();
    retainedStaticPrototypeIds.clear();

    std::vector<std::uint8_t> source;
    if (!readWholeFile("Data Files\\distantland\\statics\\static_meshes", source)) {
        return false;
    }

    ByteCursor cursor(source);
    std::unordered_map<std::uint64_t, std::uint64_t> identities;

    for (std::uint32_t staticIndex = 0; staticIndex < staticCount; ++staticIndex) {
        std::uint32_t subsetCount = 0;
        BoundingSphere staticSphere = {};
        std::uint8_t staticType = 0;
        if (!cursor.read(&subsetCount, sizeof(subsetCount)) ||
            !cursor.read(&staticSphere.radius, sizeof(staticSphere.radius)) ||
            !cursor.read(&staticSphere.center, sizeof(staticSphere.center)) ||
            !cursor.read(&staticType, sizeof(staticType))) {
            return false;
        }

        for (std::uint32_t subsetIndex = 0; subsetIndex < subsetCount; ++subsetIndex) {
            BoundingSphere sphere = {};
            D3DXVECTOR3 aabbMin = {};
            D3DXVECTOR3 aabbMax = {};
            std::uint32_t vertexCount = 0;
            std::uint32_t faceCount = 0;
            if (!cursor.read(&sphere.radius, sizeof(sphere.radius)) ||
                !cursor.read(&sphere.center, sizeof(sphere.center)) ||
                !cursor.read(&aabbMin, sizeof(aabbMin)) ||
                !cursor.read(&aabbMax, sizeof(aabbMax)) ||
                !cursor.read(&vertexCount, sizeof(vertexCount)) ||
                !cursor.read(&faceCount, sizeof(faceCount))) {
                return false;
            }

            std::uint32_t vertexBytes = 0;
            std::uint32_t indexBytes = 0;
            if (!checkedBytes(vertexCount, SIZEOFSTATICVERT, vertexBytes) ||
                !checkedBytes(faceCount, 6, indexBytes)) {
                return false;
            }
            const auto* vertices = cursor.take(vertexBytes);
            const auto* indices = cursor.take(indexBytes);
            std::uint8_t textureFlags[2] = {};
            std::uint16_t pathBytes = 0;
            if ((!vertices && vertexBytes) || (!indices && indexBytes) ||
                !cursor.read(textureFlags, sizeof(textureFlags)) ||
                !cursor.read(&pathBytes, sizeof(pathBytes))) {
                return false;
            }
            const auto* texturePath = cursor.take(pathBytes);
            if (!texturePath && pathBytes) {
                return false;
            }

            std::uint64_t identity = hashBytes(&staticType, sizeof(staticType));
            identity = hashBytes(&sphere, sizeof(sphere), identity);
            identity = hashBytes(&aabbMin, sizeof(aabbMin), identity);
            identity = hashBytes(&aabbMax, sizeof(aabbMax), identity);
            identity = hashBytes(vertices, vertexBytes, identity);
            identity = hashBytes(indices, indexBytes, identity);
            identity = hashBytes(textureFlags, sizeof(textureFlags), identity);
            identity = hashBytes(texturePath, pathBytes, identity);

            std::uint64_t witness = hashBytes(&staticType, sizeof(staticType), 1099511628211ull);
            witness = hashBytes(&sphere, sizeof(sphere), witness);
            witness = hashBytes(&aabbMin, sizeof(aabbMin), witness);
            witness = hashBytes(&aabbMax, sizeof(aabbMax), witness);
            witness = hashBytes(vertices, vertexBytes, witness);
            witness = hashBytes(indices, indexBytes, witness);
            witness = hashBytes(textureFlags, sizeof(textureFlags), witness);
            witness = hashBytes(texturePath, pathBytes, witness);
            if (identity == 0) {
                return false;
            }

            retainedStaticPrototypeIds.push_back(identity);
            const auto [known, inserted] = identities.emplace(identity, witness);
            if (!inserted) {
                if (known->second != witness) {
                    return false;
                }
                continue;
            }

            RetainedCatalog::Mesh mesh = {};
            mesh.identity = identity;
            mesh.materialIdentity = hashBytes(texturePath, pathBytes);
            mesh.category = RetainedCatalog::Category::Static;
            mesh.flags = (textureFlags[0] ? RetainedCatalog::MeshFlagHasAlpha : 0) |
                         (textureFlags[1] ? RetainedCatalog::MeshFlagAnimatedUv : 0);
            mesh.vertexCount = vertexCount;
            mesh.vertexStride = SIZEOFSTATICVERT;
            mesh.vertexBytes = vertexBytes;
            mesh.indexCount = faceCount * 3;
            mesh.indexStride = 2;
            mesh.indexBytes = indexBytes;
            mesh.materialBytes = pathBytes;
            if (!appendBytes(retainedStaticBlob, vertices, vertexBytes, mesh.vertexOffset) ||
                !appendBytes(retainedStaticBlob, indices, indexBytes, mesh.indexOffset) ||
                !appendBytes(retainedStaticBlob, texturePath, pathBytes, mesh.materialOffset)) {
                return false;
            }

            retainedStaticMeshes.push_back(mesh);
        }
    }

    if (!cursor.atEnd()) {
        return false;
    }

    ++retainedCatalogGeneration;
    return true;
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
    retainedTerrainMeshes.clear();
    retainedTerrainBlob.clear();

    HANDLE file = CreateFile("Data Files\\distantland\\world", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        LOG::winerror("Server failed to open landscape data");
        return false;
    }

    bool readingBuffers = false;
    const auto fail = [&]() {
        if (readingBuffers) {
            landscapeBuffers.end_read();
            readingBuffers = false;
        }
        CloseHandle(file);
        retainedTerrainMeshes.clear();
        retainedTerrainBlob.clear();
        return false;
    };

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file, &fileSize) ||
        fileSize.QuadPart < static_cast<LONGLONG>(sizeof(DWORD)) ||
        fileSize.QuadPart > static_cast<LONGLONG>(std::numeric_limits<DWORD>::max())) {
        return fail();
    }
    const DWORD file_size = static_cast<DWORD>(fileSize.QuadPart);
    std::uint64_t bytesConsumed = 0;
    const auto readExact = [&](void* destination, DWORD bytes) {
        if (bytes > static_cast<std::uint64_t>(file_size) - bytesConsumed) {
            return false;
        }
        DWORD bytesRead = 0;
        if (bytes != 0 &&
            (!ReadFile(file, destination, bytes, &bytesRead, nullptr) || bytesRead != bytes)) {
            return false;
        }
        bytesConsumed += bytes;
        return true;
    };

    DWORD mesh_count = 0;
    if (!readExact(&mesh_count, sizeof(mesh_count))) {
        return fail();
    }

    constexpr std::uint64_t meshMetadataBytes =
        sizeof(float) + 3 * sizeof(D3DXVECTOR3) + 2 * sizeof(DWORD);
    if (mesh_count > landscapeBuffers.max_size() ||
        static_cast<std::uint64_t>(mesh_count) * meshMetadataBytes >
            static_cast<std::uint64_t>(file_size) - bytesConsumed) {
        return fail();
    }

    vector<LandMesh> meshesLand(mesh_count);
    landscapeBuffers.start_read();
    readingBuffers = true;
    auto it = landscapeBuffers.begin();
    if (!meshesLand.empty()) {
        D3DXVECTOR2 qtmin(FLT_MAX, FLT_MAX), qtmax(-FLT_MAX, -FLT_MAX);
        D3DXMATRIX world;
        D3DXMatrixIdentity(&world);
        std::unordered_set<std::uint64_t> terrainIdentities;

        const auto finiteVector = [](const D3DXVECTOR3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };

        // Load meshes and calculate max size of quadtree
        for (auto& i : meshesLand) {
            D3DXVECTOR3 boxMin, boxMax;
            if (!readExact(&i.sphere.radius, sizeof(i.sphere.radius)) ||
                !readExact(&i.sphere.center, sizeof(i.sphere.center)) ||
                !readExact(&boxMin, sizeof(boxMin)) ||
                !readExact(&boxMax, sizeof(boxMax)) ||
                !readExact(&i.verts, sizeof(i.verts)) ||
                !readExact(&i.faces, sizeof(i.faces))) {
                return fail();
            }
            if (!std::isfinite(i.sphere.radius) || i.sphere.radius < 0.0f ||
                !finiteVector(i.sphere.center) || !finiteVector(boxMin) || !finiteVector(boxMax) ||
                boxMin.x > boxMax.x || boxMin.y > boxMax.y || boxMin.z > boxMax.z ||
                !std::isfinite(i.sphere.center.x - i.sphere.radius) ||
                !std::isfinite(i.sphere.center.x + i.sphere.radius) ||
                !std::isfinite(i.sphere.center.y - i.sphere.radius) ||
                !std::isfinite(i.sphere.center.y + i.sphere.radius)) {
                return fail();
            }
            i.box.Set(boxMin, boxMax);

            std::uint32_t indexCount = 0;
            std::uint32_t vertexBytes = 0;
            std::uint32_t indexBytes = 0;
            const bool large = (i.verts > 0xFFFF || i.faces > 0xFFFF);
            if (!checkedBytes(i.faces, 3, indexCount) ||
                !checkedBytes(i.verts, SIZEOFLANDVERT, vertexBytes) ||
                !checkedBytes(indexCount, large ? 4u : 2u, indexBytes) ||
                static_cast<std::uint64_t>(vertexBytes) + indexBytes >
                    static_cast<std::uint64_t>(file_size) - bytesConsumed) {
                return fail();
            }

            const double cellXd = std::floor(static_cast<double>(i.sphere.center.x) / 8192.0);
            const double cellYd = std::floor(static_cast<double>(i.sphere.center.y) / 8192.0);
            if (cellXd < std::numeric_limits<std::int32_t>::min() ||
                cellXd > std::numeric_limits<std::int32_t>::max() ||
                cellYd < std::numeric_limits<std::int32_t>::min() ||
                cellYd > std::numeric_limits<std::int32_t>::max()) {
                return fail();
            }

            std::vector<std::uint8_t> vertices(vertexBytes);
            std::vector<std::uint8_t> indices(indexBytes);
            if (!readExact(vertices.data(), vertexBytes) ||
                !readExact(indices.data(), indexBytes)) {
                return fail();
            }

            const std::int32_t cellX = static_cast<std::int32_t>(cellXd);
            const std::int32_t cellY = static_cast<std::int32_t>(cellYd);
            std::uint64_t identity = hashBytes(&cellX, sizeof(cellX));
            identity = hashBytes(&cellY, sizeof(cellY), identity);
            identity = hashBytes(vertices.data(), vertices.size(), identity);
            identity = hashBytes(indices.data(), indices.size(), identity);
            if (identity == 0 || !terrainIdentities.insert(identity).second) {
                return fail();
            }

            RetainedCatalog::Mesh catalogMesh = {};
            catalogMesh.identity = identity;
            catalogMesh.category = RetainedCatalog::Category::Terrain;
            catalogMesh.flags = large ? RetainedCatalog::MeshFlagIndex32 : RetainedCatalog::MeshFlagNone;
            catalogMesh.vertexBytes = vertexBytes;
            catalogMesh.vertexCount = i.verts;
            catalogMesh.vertexStride = SIZEOFLANDVERT;
            catalogMesh.indexBytes = indexBytes;
            catalogMesh.indexCount = indexCount;
            catalogMesh.indexStride = large ? 4 : 2;
            catalogMesh.cellX = cellX;
            catalogMesh.cellY = cellY;
            if (!appendBytes(retainedTerrainBlob, vertices.data(), vertexBytes, catalogMesh.vertexOffset) ||
                !appendBytes(retainedTerrainBlob, indices.data(), indexBytes, catalogMesh.indexOffset)) {
                return fail();
            }
            retainedTerrainMeshes.push_back(catalogMesh);

            if (it.at_end()) {
                LOG::logline("Client landscape buffers ended while the server still has more meshes (%u buffers found, expected %u)", landscapeBuffers.size(), mesh_count);
                return fail();
            }
            auto& buffers = *it;
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
        std::size_t terrainIndex = 0;
        for (auto& i : meshesLand) {
            QuadTreeMesh* qm = LandQuadTree.AddMesh(i.sphere, i.box, world, false, false, texWorldColour, i.verts, i.vbuffer, i.faces, i.ibuffer);
            if (qm) {
                qm->cellX = (int32_t)std::floor(i.sphere.center.x / 8192.0f);
                qm->cellY = (int32_t)std::floor(i.sphere.center.y / 8192.0f);
                qm->cellValid = true;
                qm->compositeTex = 0;  // resolved per frame via the cache; never a baked handle here (ptr32, so 0 not nullptr)
                if (terrainIndex < retainedTerrainMeshes.size()) {
                    qm->retainedPrototypeIdentity = retainedTerrainMeshes[terrainIndex].identity;
                    qm->retainedPlacementIdentity = retainedTerrainMeshes[terrainIndex].identity;
                }
            }
            ++terrainIndex;
        }
    }

    landscapeBuffers.end_read();
    readingBuffers = false;
    CloseHandle(file);
    LandQuadTree.CalcVolume();

    // Log approximate memory use
    LOG::logline("-- Distant landscape memory use: %u MB", file_size / (1 << 20));

    // Architecture B Format_Loader (task 6.1): after the existing mesh table read, attempt
    // to load the additive per-cell composite set. loadCompositeSet() classifies the
    // on-disk composite.{index,data} as New_Format vs Old/absent/malformed, and on New
    // loads the full compressed pool into this 64-bit process and builds the
    // CellId/chunk indices (Req 3.2, 4.1). It never throws and returns false for the
    // Old/absent/malformed path, where the pool stays inactive so the Composite_Streamer
    // is inert and the renderer uses the Single_Atlas_Path (Req 3.3, 4.6, 6.3). The
    // distant-land load itself succeeds regardless of the composite outcome.
    loadCompositeSet();

    if (compositePool.active) {
        for (auto& mesh : retainedTerrainMeshes) {
            const auto record = compositePool.byCell.find(CellId{ mesh.cellX, mesh.cellY });
            if (record == compositePool.byCell.end()) {
                continue;
            }
            const auto& composite = record->second;
            const auto* bytes = compositePool.blob.data() + composite.poolOffset;
            mesh.materialIdentity = hashBytes(bytes, composite.byteLength);
            mesh.materialBytes = composite.byteLength;
            mesh.flags |= RetainedCatalog::MeshFlagCompositeDxt1;
            if (!appendBytes(retainedTerrainBlob, bytes, composite.byteLength, mesh.materialOffset)) {
                retainedTerrainMeshes.clear();
                retainedTerrainBlob.clear();
                return false;
            }
        }
    }

    ++retainedCatalogGeneration;
    return true;
}

bool DistantLandShare::setCurrentWorldSpace(const char* name) {
    auto it = mapWorldSpaces.find(name);
    if (it != mapWorldSpaces.end()) {
        const auto* nextWorldSpace = &it->second;
        if (currentWorldSpace != nextWorldSpace) {
            ++retainedCatalogGeneration;
        }
        currentWorldSpace = nextWorldSpace;
        hasCurrentWorldSpace = true;
        return true;
    }

    if (currentWorldSpace != nullptr || hasCurrentWorldSpace) {
        ++retainedCatalogGeneration;
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

bool DistantLandShare::writeRetainedCatalog(
    IPC::Vec<RetainedCatalog::Header>& outHeader,
    IPC::Vec<RetainedCatalog::Cell>& outCells,
    IPC::Vec<RetainedCatalog::Mesh>& outMeshes,
    IPC::Vec<RetainedCatalog::Placement>& outPlacements,
    IPC::Vec<std::uint8_t>& outBlob) {
    outHeader.truncate(0);
    outCells.truncate(0);
    outMeshes.truncate(0);
    outPlacements.truncate(0);
    outBlob.truncate(0);

    if (!hasCurrentWorldSpace || !currentWorldSpace || retainedTerrainMeshes.empty()) {
        return false;
    }

    struct CellBuild {
        std::vector<RetainedCatalog::Mesh> terrain;
        std::vector<RetainedCatalog::Placement> placements;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, CellBuild> byCell;

    for (const auto& mesh : retainedTerrainMeshes) {
        byCell[{ mesh.cellX, mesh.cellY }].terrain.push_back(mesh);
    }

    std::unordered_map<std::uint64_t, const RetainedCatalog::Mesh*> prototypes;
    for (const auto& mesh : retainedStaticMeshes) {
        prototypes.emplace(mesh.identity, &mesh);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> placementWitnesses;

    auto collectStatics = [&](const std::unique_ptr<QuadTree>& tree) {
        if (!tree) {
            return true;
        }
        bool valid = true;
        tree->ForEachMesh([&](const QuadTreeMesh& mesh) {
            if (!valid || !mesh.cellValid) {
                return;
            }
            const auto cell = byCell.find({ mesh.cellX, mesh.cellY });
            if (cell == byCell.end()) {
                return;
            }
            if (mesh.retainedPrototypeIdentity == 0 ||
                mesh.retainedPlacementIdentity == 0 ||
                prototypes.find(mesh.retainedPrototypeIdentity) == prototypes.end()) {
                valid = false;
                return;
            }

            RetainedCatalog::Placement placement = {};
            placement.identity = mesh.retainedPlacementIdentity;
            placement.prototypeIdentity = mesh.retainedPrototypeIdentity;
            std::memcpy(placement.transform, &mesh.transform, sizeof(placement.transform));
            placement.cellX = mesh.cellX;
            placement.cellY = mesh.cellY;
            placement.flags = mesh.enabled ? 1u : 0u;

            std::uint64_t witness = hashBytes(
                &placement.prototypeIdentity,
                sizeof(placement.prototypeIdentity),
                1099511628211ull);
            witness = hashBytes(placement.transform, sizeof(placement.transform), witness);
            witness = hashBytes(&placement.cellX, sizeof(placement.cellX), witness);
            witness = hashBytes(&placement.cellY, sizeof(placement.cellY), witness);
            const auto [known, inserted] = placementWitnesses.emplace(placement.identity, witness);
            if (!inserted) {
                if (known->second != witness) {
                    valid = false;
                }
                return;
            }

            cell->second.placements.push_back(placement);
        });
        return valid;
    };

    // Complete-tree traversal is intentional: frustum visibility never owns retained lifetime.
    if (!collectStatics(currentWorldSpace->NearStatics) ||
        !collectStatics(currentWorldSpace->FarStatics) ||
        !collectStatics(currentWorldSpace->VeryFarStatics)) {
        return false;
    }

    std::vector<RetainedCatalog::Cell> cells;
    std::vector<RetainedCatalog::Mesh> meshes;
    std::vector<RetainedCatalog::Placement> placements;
    cells.reserve(byCell.size());

    for (auto& [coordinates, build] : byCell) {
        std::sort(build.terrain.begin(), build.terrain.end(), [](const auto& a, const auto& b) {
            return a.identity < b.identity;
        });
        std::sort(build.placements.begin(), build.placements.end(), [](const auto& a, const auto& b) {
            return a.identity < b.identity;
        });

        RetainedCatalog::Cell cell = {};
        cell.cellX = coordinates.first;
        cell.cellY = coordinates.second;
        cell.terrainMeshFirst = static_cast<std::uint32_t>(meshes.size());
        cell.terrainMeshCount = static_cast<std::uint32_t>(build.terrain.size());
        meshes.insert(meshes.end(), build.terrain.begin(), build.terrain.end());
        cell.staticPlacementFirst = static_cast<std::uint32_t>(placements.size());
        cell.staticPlacementCount = static_cast<std::uint32_t>(build.placements.size());
        placements.insert(placements.end(), build.placements.begin(), build.placements.end());
        cells.push_back(cell);
    }

    std::unordered_set<std::uint64_t> referencedPrototypes;
    for (const auto& placement : placements) {
        referencedPrototypes.insert(placement.prototypeIdentity);
    }
    std::vector<std::uint64_t> sortedPrototypeIds(
        referencedPrototypes.begin(), referencedPrototypes.end());
    std::sort(sortedPrototypeIds.begin(), sortedPrototypeIds.end());

    std::vector<std::uint8_t> blob = retainedTerrainBlob;
    const auto copyStaticSpan = [&](std::uint32_t sourceOffset,
                                    std::uint32_t bytes,
                                    std::uint32_t& destinationOffset) {
        if (sourceOffset > retainedStaticBlob.size() ||
            bytes > retainedStaticBlob.size() - sourceOffset) {
            return false;
        }
        const auto* source = bytes == 0
            ? nullptr
            : retainedStaticBlob.data() + sourceOffset;
        return appendBytes(blob, source, bytes, destinationOffset);
    };

    for (const auto identity : sortedPrototypeIds) {
        const auto& sourceMesh = *prototypes.at(identity);
        auto mesh = sourceMesh;
        if (!copyStaticSpan(sourceMesh.vertexOffset, sourceMesh.vertexBytes, mesh.vertexOffset) ||
            !copyStaticSpan(sourceMesh.indexOffset, sourceMesh.indexBytes, mesh.indexOffset) ||
            !copyStaticSpan(sourceMesh.materialOffset, sourceMesh.materialBytes, mesh.materialOffset)) {
            return false;
        }
        meshes.push_back(mesh);
    }

    if (cells.size() > std::numeric_limits<std::uint32_t>::max() ||
        meshes.size() > std::numeric_limits<std::uint32_t>::max() ||
        placements.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    std::uint64_t contentHash = hashBytes(cells.data(), cells.size() * sizeof(cells[0]));
    contentHash = hashBytes(meshes.data(), meshes.size() * sizeof(meshes[0]), contentHash);
    contentHash = hashBytes(placements.data(), placements.size() * sizeof(placements[0]), contentHash);
    contentHash = hashBytes(blob.data(), blob.size(), contentHash);

    RetainedCatalog::Header header = {};
    header.magic = RetainedCatalog::Magic;
    header.version = RetainedCatalog::Version;
    header.headerBytes = sizeof(header);
    header.generation = retainedCatalogGeneration;
    header.contentHash = contentHash;
    header.cellCount = static_cast<std::uint32_t>(cells.size());
    header.meshCount = static_cast<std::uint32_t>(meshes.size());
    header.placementCount = static_cast<std::uint32_t>(placements.size());
    header.blobBytes = static_cast<std::uint32_t>(blob.size());

    return publishRetainedCatalog(
        header,
        cells,
        meshes,
        placements,
        blob,
        outHeader,
        outCells,
        outMeshes,
        outPlacements,
        outBlob);
}
