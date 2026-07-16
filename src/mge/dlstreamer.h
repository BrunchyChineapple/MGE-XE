#pragma once

#include "mge/dlcomposite.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Shared server-side composite-pool types for Architecture B (per-cell distant-land
// composite texturing). These are the foundation the Format_Loader (task 6.1,
// ipc/dlshare.cpp initLandscapeServer) populates and the Composite_Streamer
// (task 6.2, dlstreamer.cpp CompositeStreamerServer) consumes.
//
// Everything here is pure data: CellId / CompositeRecord carry no D3D handles and no
// runtime pointers, and CompositePool owns only a byte blob plus index maps. That keeps
// the header compilable in BOTH processes that build ipc/dlshare.cpp -- the 64-bit host
// (mgeHost64.exe, MGE64_HOST) where the pool actually lives, and the 32-bit client
// (d3d8.dll) where the same translation unit compiles but the pool stays inert (the
// client owns its own resident upload cache instead, task 8.4). No exceptions are
// raised by anything here; the loader treats every malformed input as Old/absent and
// falls back to the Single_Atlas_Path (Requirements 4.6, 6.3).

// CellId / CellIdHash now live in mge/dlcomposite.h (included above), the single canonical
// definition shared by the server pool, the client cache, and the reconcile loop. They were
// duplicated here originally; they were unified into dlcomposite.h (task 8.5) because
// distantland.h pulls in both this header and compositecache.h in one TU, and two global
// `struct CellId` definitions are a C2011 redefinition. The layout is unchanged: { int32_t
// x; int32_t y; } keyed by the same 64-bit mix hash.

// One record per exterior cell present in the loaded composite directory. poolOffset and
// byteLength slice this cell's compressed DXT1 (+ mip chain) bytes out of
// CompositePool::blob; isPlaceholder marks a cell the generator failed to composite and
// backfilled (Requirement 1.7) -- it is still bindable, never dropped.
struct CompositeRecord {
    CellId   cell;
    uint32_t edgeTexels   = 0;   // this cell's baked resolution (default 1024)
    uint32_t byteLength   = 0;   // DXT1 + mips byte length within the pool blob
    uint64_t poolOffset   = 0;   // byte offset into CompositePool::blob
    bool     isPlaceholder = false;
};

// The full compressed composite pool held in the server's 64-bit address space, plus the
// two indices the streamer needs: CellId -> CompositeRecord (slice lookup) and
// chunkIndex -> CellId (resolve a distant-land mesh chunk to its cell). active is true
// only after a structurally complete New_Format set has been validated and loaded; it
// stays false (and the maps/blob stay empty) for every Old/absent/malformed input, which
// is the signal that the Composite_Streamer must be inert (Requirement 4.6).
struct CompositePool {
    bool     active = false;            // true only for a valid, fully-loaded New_Format set
    uint32_t cellCount = 0;             // CompositeSetHeader::cellCount
    uint32_t defaultEdgeTexels = 0;     // CompositeSetHeader::defaultEdgeTexels
    uint32_t flags = 0;                 // CompositeSetHeader::flags (bit0 = has placeholders)

    std::vector<uint8_t> blob;          // full composite.data, loaded into 64-bit memory
    std::unordered_map<CellId, CompositeRecord, CellIdHash> byCell;  // CellId -> record
    std::unordered_map<uint32_t, CellId> byChunk;                    // chunkIndex -> CellId

    // Reset to the inactive (single-atlas-only) state, releasing the pool memory.
    void clear() {
        active = false;
        cellCount = 0;
        defaultEdgeTexels = 0;
        flags = 0;
        blob.clear();
        blob.shrink_to_fit();
        byCell.clear();
        byChunk.clear();
    }
};

// NOTE (task 6.2): CompositeStreamerServer -- the class that owns a reference to this
// CompositePool, reports isActive() (false for Old_Format), and fills an
// IPC::Vec<CompositeChunkMsg> for cells newly entering the Visible_Cell_Set -- is added
// in dlstreamer.cpp/.h by task 6.2. It builds directly on the CompositePool above; task
// 6.1 only populates the pool and surfaces its shape through InitLandscapeParameters.

// ---------------------------------------------------------------------------------------
// CompositeStreamerServer (task 6.2): the 64-bit server half of the Composite_Streamer.
// ---------------------------------------------------------------------------------------
//
// Forward declarations keep this header light: the IPC vector machinery (ipc/vec.h, with
// its template definitions) and the on-the-wire CompositeChunkMsg (ipc/bridge.h) are only
// pulled in by dlstreamer.cpp, not by every server translation unit that includes
// dlshare.h. A reference/pointer to an incomplete IPC::Vec<T> is legal in a declaration;
// the full type is needed only at the call site, which lives in the .cpp.
struct CompositeChunkMsg;
enum class CompositeBatchStatus : std::uint32_t;
namespace IPC { template<typename T> class Vec; }

// The Visible_Cell_Set delta the client hands the server each frame: the list of cells
// newly entering the visible set (newVisible \ alreadyResident; the client computes the
// set difference -- the server streams exactly what it is asked for, Requirement 4.2).
// A list of CellId, decoupled from the IPC transport type so the streamer stays pure and
// unit-testable; the server command handler bridges the on-the-wire delta vector into one
// of these before calling streamNewlyVisible.
using VisibleCellDelta = std::vector<CellId>;

// Owns a reference to the loaded CompositePool (DistantLandShare::compositePool, populated
// by the task 6.1 Format_Loader) and streams only the newly-visible cells' compressed DXT1
// bytes to the client. The pool already lives in the 64-bit address space, so there is no
// init()/load step here -- the streamer is a thin, stateless view over the pool, exactly as
// the task 6.1 hand-off note specified. Non-visible cells are never touched, so they stay
// resident only in the server pool (Requirement 4.4).
class CompositeStreamerServer {
public:
    explicit CompositeStreamerServer(const CompositePool& pool) : pool_(pool) {}

    // false for an Old_Format / absent / malformed load (the pool never went active), which
    // makes streamNewlyVisible inert and keeps the client on the Single_Atlas_Path (Req 4.6).
    bool isActive() const { return pool_.active; }

    // Emit exactly one explicit outcome per requested cell. Found payloads are limited by the
    // actual server-known byte lengths; deferred cells carry no bytes and are retryable.
    CompositeBatchStatus streamNewlyVisible(
        const VisibleCellDelta& delta,
        IPC::Vec<CompositeChunkMsg>& out,
        IPC::Vec<std::uint8_t>& outBytes) const;

private:
    const CompositePool& pool_;
};
