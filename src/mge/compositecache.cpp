
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
constexpr std::uint64_t kDXT1BlockBytes = 8;
constexpr std::uint64_t kBytesPerMB = 1024ull * 1024ull;

bool checkedMultiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return false;
    }
    result = a * b;
    return true;
}

bool checkedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        return false;
    }
    result = a + b;
    return true;
}

bool dxt1LevelBytes(std::uint32_t w, std::uint32_t h, std::uint64_t& result) {
    const std::uint64_t blocksW = std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(w) + 3) / 4);
    const std::uint64_t blocksH = std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(h) + 3) / 4);
    std::uint64_t blockCount = 0;
    return checkedMultiply(blocksW, blocksH, blockCount) &&
           checkedMultiply(blockCount, kDXT1BlockBytes, result);
}

std::uint32_t mipLevelCount(std::uint32_t edge) {
    std::uint32_t levels = 1;
    while (edge > 1) {
        edge >>= 1;
        ++levels;
    }
    return levels;
}

bool dxt1FullChainBytes(std::uint32_t edge, std::uint64_t& result) {
    if (edge == 0) {
        return false;
    }

    result = 0;
    std::uint32_t w = edge;
    std::uint32_t h = edge;
    for (;;) {
        std::uint64_t levelBytes = 0;
        if (!dxt1LevelBytes(w, h, levelBytes) ||
            !checkedAdd(result, levelBytes, result)) {
            return false;
        }
        if (w == 1 && h == 1) {
            return true;
        }
        w = std::max<std::uint32_t>(1, w >> 1);
        h = std::max<std::uint32_t>(1, h >> 1);
    }
}

bool payloadShape(const CompositeChunkMsg& msg, std::uint32_t& levels) {
    if (msg.status != CompositeCellStatus::Found ||
        msg.edgeTexels == 0 || msg.edgeTexels > kCompositeMaxEdgeTexels ||
        msg.byteLength == 0 || msg.byteLength > kCompositeMaxPayloadBytes) {
        return false;
    }

    std::uint64_t fullChain = 0;
    std::uint64_t baseOnly = 0;
    if (!dxt1FullChainBytes(msg.edgeTexels, fullChain) ||
        !dxt1LevelBytes(msg.edgeTexels, msg.edgeTexels, baseOnly)) {
        return false;
    }

    if (msg.byteLength == fullChain) {
        levels = mipLevelCount(msg.edgeTexels);
        return true;
    }
    if (msg.byteLength == baseOnly) {
        levels = 1;
        return true;
    }
    return false;
}

}  // namespace

ResidentCompositeCache::ResidentCompositeCache()
    : device_(nullptr),
      generation_(0),
      budgetBytes_(0),
      residentBytes_(0),
      atlasServedThisFrame_(0) {}

ResidentCompositeCache::~ResidentCompositeCache() {
    releaseAll();
}

void ResidentCompositeCache::init(IDirect3DDevice9* device) {
    device_ = device;
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
}

void ResidentCompositeCache::setBudgetMB(std::uint32_t mb) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(mb) * kBytesPerMB;
    budgetBytes_ = bytes > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(bytes);
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

// Exact resident byte count for a square DXT1 texture including every mip level. Small
// levels still occupy one complete 4x4 block.
std::uint32_t ResidentCompositeCache::compositeBytes(std::uint32_t edgeTexels) {
    std::uint64_t bytes = 0;
    if (!dxt1FullChainBytes(edgeTexels, bytes) ||
        bytes > std::numeric_limits<std::uint32_t>::max()) {
        return edgeTexels == 0 ? 0 : std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(bytes);
}

bool ResidentCompositeCache::validatePayload(const CompositeChunkMsg& msg) {
    std::uint32_t levels = 0;
    return payloadShape(msg, levels);
}

bool ResidentCompositeCache::uploadDXT1(const CompositeChunkMsg& msg,
                                        const std::uint8_t* dxt1Bytes,
                                        IDirect3DTexture9** outTex) const {
    *outTex = nullptr;

    if (device_ == nullptr || dxt1Bytes == nullptr) {
        return false;
    }

    std::uint32_t levels = 0;
    if (!payloadShape(msg, levels)) {
        LOG::logline(
            "!! ResidentCompositeCache: invalid DXT1 payload for cell (%d,%d): edge=%u bytes=%u",
            msg.cellX, msg.cellY, msg.edgeTexels, msg.byteLength);
        return false;
    }
    const std::uint32_t edge = msg.edgeTexels;

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

CompositeAdmissionResult ResidentCompositeCache::admitWithResult(
    const CompositeChunkMsg& msg,
    const std::uint8_t* dxt1Bytes) {
    const CellId cell{ msg.cellX, msg.cellY };

    auto existing = resident_.find(cell);
    if (existing != resident_.end()) {
        existing->second.evictionPending = false;
        return CompositeAdmissionResult::AlreadyResident;
    }

    if (dxt1Bytes == nullptr || !validatePayload(msg)) {
        return CompositeAdmissionResult::UploadFailed;
    }

    const std::uint32_t cellBytes = msg.byteLength;
    const std::uint64_t admissionBudget = static_cast<std::uint64_t>(budgetBytes_) +
        (isTransitionTarget(cell) ? cellBytes : 0u);
    if (static_cast<std::uint64_t>(residentBytes_) + cellBytes > admissionBudget) {
        return CompositeAdmissionResult::BudgetRejected;
    }

    IDirect3DTexture9* tex = nullptr;
    if (!uploadDXT1(msg, dxt1Bytes, &tex)) {
        return CompositeAdmissionResult::UploadFailed;
    }

    ResidentComposite rc = {};
    rc.cell = cell;
    rc.tex = tex;
    rc.bytes = cellBytes;
    resident_.emplace(cell, rc);
    residentBytes_ += cellBytes;
    return CompositeAdmissionResult::Admitted;
}

bool ResidentCompositeCache::admit(const CompositeChunkMsg& msg,
                                   const std::uint8_t* dxt1Bytes) {
    return compositeAdmissionSucceeded(admitWithResult(msg, dxt1Bytes));
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
