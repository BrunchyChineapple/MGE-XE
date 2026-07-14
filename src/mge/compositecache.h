#pragma once

#include "ipc/bridge.h"   // CompositeChunkMsg, ptr32<>, IDirect3D* (via proxydx/d3d9header.h)
#include "mge/dlcomposite.h"   // canonical CellId / CellIdHash (shared with the server pool)

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// ResidentCompositeCache (Architecture B, IPC client side / d3d8.dll, 32-bit).
//
// The 64-bit server (mgeHost64.exe) holds the full pool of compressed Per_Cell_Composite
// bytes and streams only the visible cells' DXT1 blobs to this 32-bit client. This cache
// owns the bounded resident set of uploaded composites on the D3D9 device that feeds Remix:
//   - admit()          uploads one streamed cell's DXT1 + mip chain, gated by the byte budget
//   - lookup()         resolves a cell to its resident texture (null => binder uses the atlas)
//   - evictNotVisible()releases the D3D textures of cells that left the Visible_Cell_Set
//   - setBudgetMB()    configures the Texture_Memory_Budget
//   - residentCount/residentBytes/atlasServedThisFrame  feed Composite_Telemetry
//
// The byte-budget admission rule is the testable core and is kept exact and self-consistent
// with the authoritative model tests/composite_stream_model.py:
//   cellBytes     = edgeTexels^2 / 2 * 1.333        (DXT1 base level + full mip chain)
//   budgetBytes   = budgetMB * 1 MiB
//   admit iff       residentBytes + cellBytes <= budgetBytes   (non-strict)
// A cell that would exceed the budget is NOT uploaded; admit() returns false so the
// Texture_Binder falls back to the global world.dds atlas for that cell (Req 7.3).
//
// This is client-only code, so ptr32<IDirect3DTexture9> resolves to a plain
// IDirect3DTexture9* here; the cache stores raw device pointers it owns a reference to.

// CellId / CellIdHash are defined once in mge/dlcomposite.h (included above) and shared by
// the server pool index (mge/dlstreamer.h) and tests/composite_stream_model.py. They were
// duplicated here originally; unifying them (task 8.5) is required because distantland.h
// includes both this header and dlstreamer.h in one TU. CellId is { int32_t x; int32_t y; }
// keyed by a 64-bit mix hash — the same identity the server pool and the model use.

// The Visible_Cell_Set for a frame: the cells the renderer needs resident this frame. The
// client derives it from the existing cull (frustum + Draw_Distance * kCellSize) in task 8.5.
using VisibleCellSet = std::unordered_set<CellId, CellIdHash>;

class ResidentCompositeCache {
public:
    ResidentCompositeCache();
    ~ResidentCompositeCache();

    // Capture the D3D9 device that feeds Remix (DistantLand::device). Uploads target it.
    void init(IDirect3DDevice9* device);

    // Configure the Texture_Memory_Budget (Req 7.2). Does not retroactively evict; eviction is
    // visibility-driven (evictNotVisible). budgetBytes = mb * 1 MiB.
    void setBudgetMB(std::uint32_t mb);

    // Upload one newly-streamed cell's DXT1 + mip chain to the device, enforcing the byte
    // budget. Returns true if the cell is resident after the call (admitted now, or already
    // resident). Returns false if the cell would exceed the budget (NOT uploaded -> atlas
    // fallback, Req 7.3) or if a CreateTexture / LockRect / UpdateTexture step fails
    // (Req 6.4, 4.3). dxt1Bytes points at the compressed blob (byteLength, incl. mips) that
    // followed msg in the shared-memory window.
    bool admit(const CompositeChunkMsg& msg, const std::uint8_t* dxt1Bytes);

    // Resolve a cell to its resident composite texture, or null if it is not resident (which
    // includes cells that were rejected for exceeding the budget, since those are never made
    // resident). A null return tells the Texture_Binder to use the atlas (Req 5.2, 7.3).
    IDirect3DTexture9* lookup(CellId cell) const;

    // Release the D3D textures of every resident cell absent from the new Visible_Cell_Set
    // (Req 4.5). Their compressed bytes stay resident only in the 64-bit server pool. After
    // this call the resident set is a subset of the visible set.
    void evictNotVisible(const VisibleCellSet& visible);

    // Release every resident texture (device reset / shutdown).
    void releaseAll();

    // Telemetry accessors (Composite_Telemetry; design Property 13).
    std::uint32_t residentCount() const;   // Req 8.1
    std::uint32_t residentBytes() const;   // Req 8.2 source
    std::uint32_t atlasServedThisFrame() const;  // Req 8.4

    // Set the per-frame Single_Atlas_Path-served tally (visible cells not resident this frame).
    // The client reconcile loop (task 8.5) sets this as visibleCount - residentCount(), matching
    // composite_stream_model's atlas_served_count.
    void setAtlasServedThisFrame(std::uint32_t count);

    // Budget helpers (also exercised by the budget tests). Public + static so the worst-case
    // budget unit test (task 8.6) can call compositeBytes() without a device.
    static std::uint32_t compositeBytes(std::uint32_t edgeTexels);
    std::uint32_t budgetBytes() const { return budgetBytes_; }

private:
    // One uploaded composite resident on the device.
    struct ResidentComposite {
        CellId cell;
        IDirect3DTexture9* tex;     // uploaded DXT1 + mips (default pool), owned reference
        std::uint32_t bytes;        // GPU footprint estimate = compositeBytes(edgeTexels)
        std::uint32_t lastSeenFrame;// reserved for future LRU; eviction here is visibility-driven
    };

    // Upload the precompressed DXT1 + mip chain into a default-pool texture via a system-memory
    // staging texture (the project's CreateTexture + LockRect + UpdateTexture idiom). Returns
    // false on any CreateTexture / LockRect / UpdateTexture failure or a short/invalid blob.
    bool uploadDXT1(const CompositeChunkMsg& msg, const std::uint8_t* dxt1Bytes,
                    IDirect3DTexture9** outTex) const;

    IDirect3DDevice9* device_;
    std::uint32_t budgetBytes_;
    std::uint32_t residentBytes_;
    std::uint32_t atlasServedThisFrame_;
    std::unordered_map<CellId, ResidentComposite, CellIdHash> resident_;
};
