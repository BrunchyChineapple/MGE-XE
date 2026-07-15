
#include "proxydx/d3d9header.h"
#include "support/log.h"
#include "compositecache.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

// ResidentCompositeCache: the IPC client's bounded set of uploaded Per_Cell_Composite
// textures (Architecture B). See compositecache.h for the contract; the byte-budget
// admission rule mirrors tests/composite_stream_model.py exactly.

namespace {

// DXT1 packs a 4x4 texel block into 8 bytes. Block-compressed level sizing rounds the
// dimensions up to whole 4x4 blocks.
constexpr std::uint32_t kDXT1BlockBytes = 8;

constexpr std::uint64_t kBytesPerMB = 1024ull * 1024ull;

// DXT1 byte size of one mip level of the given dimensions (rounded up to 4x4 blocks).
std::uint32_t dxt1LevelBytes(std::uint32_t w, std::uint32_t h) {
    const std::uint32_t blocksW = std::max<std::uint32_t>(1, (w + 3) / 4);
    const std::uint32_t blocksH = std::max<std::uint32_t>(1, (h + 3) / 4);
    return blocksW * blocksH * kDXT1BlockBytes;
}

// Full mip-chain level count for a square edge (down to 1x1): floor(log2(edge)) + 1.
std::uint32_t mipLevelCount(std::uint32_t edge) {
    std::uint32_t levels = 1;
    while (edge > 1) {
        edge >>= 1;
        ++levels;
    }
    return levels;
}

// Total DXT1 bytes for a square edge with a full mip chain down to 1x1.
std::uint64_t dxt1FullChainBytes(std::uint32_t edge) {
    std::uint64_t total = 0;
    std::uint32_t w = edge, h = edge;
    for (;;) {
        total += dxt1LevelBytes(w, h);
        if (w == 1 && h == 1) {
            break;
        }
        w = std::max<std::uint32_t>(1, w >> 1);
        h = std::max<std::uint32_t>(1, h >> 1);
    }
    return total;
}

}  // namespace

ResidentCompositeCache::ResidentCompositeCache()
    : device_(nullptr),
      budgetBytes_(0),
      residentBytes_(0),
      atlasServedThisFrame_(0) {}

ResidentCompositeCache::~ResidentCompositeCache() {
    releaseAll();
}

void ResidentCompositeCache::init(IDirect3DDevice9* device) {
    device_ = device;
}

void ResidentCompositeCache::setBudgetMB(std::uint32_t mb) {
    budgetBytes_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(mb) * kBytesPerMB);
}

void ResidentCompositeCache::setTransitionTargets(const VisibleCellSet& cells) {
    transitionTargets_ = cells;
}

void ResidentCompositeCache::includeTransitionTargets(VisibleCellSet& cells) const {
    cells.insert(transitionTargets_.begin(), transitionTargets_.end());
}

bool ResidentCompositeCache::isTransitionTarget(CellId cell) const {
    return transitionTargets_.find(cell) != transitionTargets_.end();
}

void ResidentCompositeCache::trimToBudget() {
    for (auto it = resident_.begin();
         it != resident_.end() && residentBytes_ > budgetBytes_;) {
        if (it->second.pins != 0 || isTransitionTarget(it->first)) {
            ++it;
            continue;
        }
        if (it->second.tex != nullptr) {
            it->second.tex->Release();
        }
        residentBytes_ -= it->second.bytes;
        it = resident_.erase(it);
    }
}

// compositeBytes - per-cell resident GPU footprint estimate: edgeTexels^2 / 2 * 1.333
// (DXT1 base level + full mip chain). Computed exactly as composite_stream_model.composite_bytes:
// integer base = (edge*edge)/2, then floor(base * 1.333). Kept identical so the budget
// admission decision is byte-for-byte consistent between the C++ and the model.
std::uint32_t ResidentCompositeCache::compositeBytes(std::uint32_t edgeTexels) {
    const std::uint64_t baseLevelBytes =
        (static_cast<std::uint64_t>(edgeTexels) * edgeTexels) / 2ull;
    return static_cast<std::uint32_t>(static_cast<double>(baseLevelBytes) * 1.333);
}

bool ResidentCompositeCache::uploadDXT1(const CompositeChunkMsg& msg,
                                        const std::uint8_t* dxt1Bytes,
                                        IDirect3DTexture9** outTex) const {
    *outTex = nullptr;

    if (device_ == nullptr || dxt1Bytes == nullptr) {
        return false;
    }
    const std::uint32_t edge = msg.edgeTexels;
    if (edge == 0 || msg.byteLength == 0) {
        return false;
    }

    // Decide the mip-level count the streamed blob actually carries. A composite is baked with
    // a full DXT1 mip chain; tolerate a base-level-only blob too. Anything else is treated as a
    // malformed blob and rejected so the binder falls back to the atlas (Req 6.4).
    std::uint32_t levels;
    const std::uint64_t fullChain = dxt1FullChainBytes(edge);
    const std::uint64_t baseOnly = dxt1LevelBytes(edge, edge);
    if (msg.byteLength == fullChain) {
        levels = mipLevelCount(edge);
    } else if (msg.byteLength == baseOnly) {
        levels = 1;
    } else {
        LOG::logline("!! ResidentCompositeCache: cell (%d,%d) blob length %u != DXT1 chain %llu/base %llu",
                     msg.cellX, msg.cellY, msg.byteLength,
                     static_cast<unsigned long long>(fullChain),
                     static_cast<unsigned long long>(baseOnly));
        return false;
    }

    // Staging texture in system memory, then UpdateTexture into a default-pool texture so the
    // resident composite lives in GPU memory the Remix path can ray-trace (the project's
    // CreateTexture + LockRect + UpdateTexture upload idiom, mirroring patchLoadTexture2D).
    IDirect3DTexture9* staging = nullptr;
    HRESULT hr = device_->CreateTexture(edge, edge, levels, 0, D3DFMT_DXT1,
                                        D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (hr != D3D_OK || staging == nullptr) {
        LOG::logline("!! ResidentCompositeCache: staging CreateTexture failed for cell (%d,%d)",
                     msg.cellX, msg.cellY);
        return false;
    }

    const std::uint8_t* src = dxt1Bytes;
    std::uint64_t consumed = 0;
    std::uint32_t w = edge, h = edge;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t blocksW = std::max<std::uint32_t>(1, (w + 3) / 4);
        const std::uint32_t blocksH = std::max<std::uint32_t>(1, (h + 3) / 4);
        const std::uint32_t srcRowBytes = blocksW * kDXT1BlockBytes;
        const std::uint32_t levelBytes = srcRowBytes * blocksH;

        // Guard against a truncated blob before reading past its end.
        if (consumed + levelBytes > msg.byteLength) {
            LOG::logline("!! ResidentCompositeCache: cell (%d,%d) truncated at mip %u",
                         msg.cellX, msg.cellY, level);
            staging->Release();
            return false;
        }

        D3DLOCKED_RECT lr;
        hr = staging->LockRect(level, &lr, nullptr, 0);
        if (hr != D3D_OK) {
            LOG::logline("!! ResidentCompositeCache: LockRect failed for cell (%d,%d) mip %u",
                         msg.cellX, msg.cellY, level);
            staging->Release();
            return false;
        }

        // Copy block-row by block-row honoring the locked pitch (it may exceed srcRowBytes).
        std::uint8_t* dst = static_cast<std::uint8_t*>(lr.pBits);
        for (std::uint32_t row = 0; row < blocksH; ++row) {
            std::memcpy(dst + static_cast<std::ptrdiff_t>(row) * lr.Pitch,
                        src + static_cast<std::ptrdiff_t>(row) * srcRowBytes,
                        srcRowBytes);
        }
        staging->UnlockRect(level);

        src += levelBytes;
        consumed += levelBytes;
        w = std::max<std::uint32_t>(1, w >> 1);
        h = std::max<std::uint32_t>(1, h >> 1);
    }

    IDirect3DTexture9* resident = nullptr;
    hr = device_->CreateTexture(edge, edge, levels, 0, D3DFMT_DXT1,
                                D3DPOOL_DEFAULT, &resident, nullptr);
    if (hr != D3D_OK || resident == nullptr) {
        LOG::logline("!! ResidentCompositeCache: default CreateTexture failed for cell (%d,%d)",
                     msg.cellX, msg.cellY);
        staging->Release();
        return false;
    }

    hr = device_->UpdateTexture(staging, resident);
    staging->Release();
    if (hr != D3D_OK) {
        LOG::logline("!! ResidentCompositeCache: UpdateTexture failed for cell (%d,%d)",
                     msg.cellX, msg.cellY);
        resident->Release();
        return false;
    }

    *outTex = resident;
    return true;
}

bool ResidentCompositeCache::admit(const CompositeChunkMsg& msg, const std::uint8_t* dxt1Bytes) {
    const CellId cell{ msg.cellX, msg.cellY };

    auto existing = resident_.find(cell);
    if (existing != resident_.end()) {
        existing->second.evictionPending = false;
        return true;
    }

    const std::uint32_t cellBytes = msg.byteLength;
    const std::uint64_t admissionBudget = static_cast<std::uint64_t>(budgetBytes_) +
        (isTransitionTarget(cell) ? cellBytes : 0u);
    if (static_cast<std::uint64_t>(residentBytes_) + cellBytes > admissionBudget) {
        return false;
    }

    IDirect3DTexture9* tex = nullptr;
    if (!uploadDXT1(msg, dxt1Bytes, &tex)) {
        return false;
    }

    ResidentComposite rc = {};
    rc.cell = cell;
    rc.tex = tex;
    rc.bytes = cellBytes;
    resident_.emplace(cell, rc);
    residentBytes_ += cellBytes;
    return true;
}

IDirect3DTexture9* ResidentCompositeCache::lookup(CellId cell) const {
    const auto it = resident_.find(cell);
    return it == resident_.end() ? nullptr : it->second.tex;
}

IDirect3DTexture9* ResidentCompositeCache::pin(CellId cell) {
    auto it = resident_.find(cell);
    if (it == resident_.end() || it->second.pins == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    ++it->second.pins;
    it->second.evictionPending = false;
    return it->second.tex;
}

void ResidentCompositeCache::unpin(CellId cell) {
    auto it = resident_.find(cell);
    if (it == resident_.end() || it->second.pins == 0) {
        LOG::logline("!! ResidentCompositeCache: unmatched unpin for cell (%d,%d)", cell.x, cell.y);
        return;
    }

    if (--it->second.pins != 0 || !it->second.evictionPending) {
        return;
    }
    if (it->second.tex != nullptr) {
        it->second.tex->Release();
    }
    residentBytes_ -= it->second.bytes;
    resident_.erase(it);
}

void ResidentCompositeCache::evictNotVisible(const VisibleCellSet& visible) {
    for (auto it = resident_.begin(); it != resident_.end();) {
        if (visible.find(it->first) != visible.end()) {
            it->second.evictionPending = false;
            ++it;
            continue;
        }
        if (it->second.pins != 0) {
            it->second.evictionPending = true;
            ++it;
            continue;
        }
        if (it->second.tex != nullptr) {
            it->second.tex->Release();
        }
        residentBytes_ -= it->second.bytes;
        it = resident_.erase(it);
    }
}

void ResidentCompositeCache::releaseAll() {
    for (auto it = resident_.begin(); it != resident_.end();) {
        if (it->second.pins != 0) {
            it->second.evictionPending = true;
            ++it;
            continue;
        }
        if (it->second.tex != nullptr) {
            it->second.tex->Release();
        }
        residentBytes_ -= it->second.bytes;
        it = resident_.erase(it);
    }
}

std::uint32_t ResidentCompositeCache::residentCount() const {
    return static_cast<std::uint32_t>(resident_.size());
}

std::uint32_t ResidentCompositeCache::visibleResidentCount(const VisibleCellSet& visible) const {
    std::uint32_t count = 0;
    for (const auto& cell : visible) {
        if (resident_.find(cell) != resident_.end()) {
            ++count;
        }
    }
    return count;
}

std::uint32_t ResidentCompositeCache::residentBytes() const {
    return residentBytes_;
}

std::uint32_t ResidentCompositeCache::atlasServedThisFrame() const {
    return atlasServedThisFrame_;
}

void ResidentCompositeCache::setAtlasServedThisFrame(std::uint32_t count) {
    atlasServedThisFrame_ = count;
}
