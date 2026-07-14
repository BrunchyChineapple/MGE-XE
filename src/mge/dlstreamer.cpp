#include "mge/dlstreamer.h"

#include "ipc/bridge.h"   // CompositeChunkMsg (the on-the-wire header record)
#include "ipc/vec.h"      // IPC::Vec<T> full definition (push_back / clear / truncate)

#include <cstdint>

// CompositeStreamerServer (task 6.2): the 64-bit server half of the Composite_Streamer.
//
// This translation unit only compiles into the host (mgeHost64.exe, MGE64_HOST) alongside
// ipc/dlshare.cpp, ipc/server.cpp, and the rest of the IPC machinery -- it is added to
// mgeHost64.vcxproj, not to the 32-bit d3d8.dll project, because the full compressed pool
// lives only in the 64-bit address space (Requirement 4.1). The client owns its own
// resident upload cache (ResidentCompositeCache, mge/compositecache.cpp) instead.
//
// The streamer is a thin, stateless view over the CompositePool that the Format_Loader
// (task 6.1, DistantLandShare::loadCompositeSet) already populated, exactly as the task 6.1
// hand-off note specified: it holds a const reference to the pool, reports isActive() off
// pool.active, and resolves each delta cell through pool.byCell. It never loads, copies, or
// mutates the pool, so cells outside the requested delta are never read and stay resident
// only in the server pool (Requirement 4.4).

void CompositeStreamerServer::streamNewlyVisible(const VisibleCellDelta& delta,
                                                 IPC::Vec<CompositeChunkMsg>& out,
                                                 IPC::Vec<std::uint8_t>* outBytes) const {
    // Always begin from empty output channels so a caller that reuses the same shared
    // vectors across frames never sees stale cells. clear() just resets the shared size.
    out.clear();
    if (outBytes != nullptr) {
        outBytes->clear();
    }

    // Inert for an Old_Format / absent / malformed load: the pool never went active, so the
    // streamer emits nothing and the client stays on the Single_Atlas_Path (Requirement 4.6).
    if (!pool_.active) {
        return;
    }

    // Stream exactly the cells the client asked for -- the delta the client computed as
    // (newVisible \ alreadyResident). The server does not recompute visibility; it answers
    // the delta (Requirement 4.2). Order is preserved so the header channel and the byte
    // channel stay aligned: the client consumes byteLength bytes per header in sequence.
    for (const CellId& cell : delta) {
        auto it = pool_.byCell.find(cell);
        if (it == pool_.byCell.end()) {
            // Not in the pool (e.g. a cell with no baked composite). Skip it; the client
            // resolves the miss to the global world.dds atlas for that chunk.
            continue;
        }
        const CompositeRecord& record = it->second;

        // Defensive bounds check on the pool slice before any copy. The Format_Loader
        // already validated every entry's span against composite.data, but the streamer
        // must never read past the blob even if an upstream invariant is ever violated.
        const std::uint64_t sliceEnd =
            record.poolOffset + static_cast<std::uint64_t>(record.byteLength);
        if (sliceEnd > pool_.blob.size()) {
            continue;
        }

        // Append this cell's header. The compressed DXT1 (+ mip chain) bytes themselves
        // travel in the parallel byte channel, because an IPC::Vec<CompositeChunkMsg> has a
        // fixed element stride and cannot inline a variable-length payload -- the design's
        // single-channel "bytes follow in the window" sketch is realized as header + byte
        // vectors written in lockstep.
        CompositeChunkMsg msg;
        msg.cellX = cell.x;
        msg.cellY = cell.y;
        msg.edgeTexels = record.edgeTexels;
        msg.byteLength = record.byteLength;

        if (!out.push_back(msg)) {
            // The header channel is full (hit its reserved maximum). Stop streaming further
            // cells this frame; the client binds the atlas for the ones it did not receive.
            break;
        }

        if (outBytes != nullptr) {
            const std::uint8_t* src = pool_.blob.data() + record.poolOffset;
            bool byteChannelFull = false;
            for (std::uint32_t i = 0; i < record.byteLength; ++i) {
                if (!outBytes->push_back(src[i])) {
                    byteChannelFull = true;
                    break;
                }
            }
            if (byteChannelFull) {
                // The byte channel filled partway through this cell. Drop the header we just
                // added so the two channels stay consistent (the client reconstructs cells by
                // reading byteLength bytes per header, so a header with missing bytes would
                // desync it). Then stop -- the dropped cell falls back to the atlas.
                out.truncate(out.size() - 1);
                break;
            }
        }
    }
}
