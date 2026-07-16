#include "mge/dlstreamer.h"

#include "ipc/bridge.h"
#include "ipc/vec.h"

#include <cstdint>

namespace {

std::uint64_t dxt1LevelBytes(std::uint32_t edgeTexels) {
    const std::uint64_t blocks =
        (static_cast<std::uint64_t>(edgeTexels) + 3u) / 4u;
    return blocks * blocks * 8u;
}

bool validDxt1PayloadShape(std::uint32_t edgeTexels, std::uint32_t byteLength) {
    if (edgeTexels == 0 || edgeTexels > kCompositeMaxEdgeTexels ||
        byteLength == 0 || byteLength > kCompositeMaxPayloadBytes) {
        return false;
    }

    const std::uint64_t baseOnly = dxt1LevelBytes(edgeTexels);
    std::uint64_t fullChain = 0;
    for (std::uint32_t edge = edgeTexels;; edge = edge > 1 ? edge >> 1 : 1) {
        fullChain += dxt1LevelBytes(edge);
        if (edge == 1) {
            break;
        }
    }

    return byteLength == baseOnly || byteLength == fullChain;
}

CompositeChunkMsg makeOutcome(CellId cell, CompositeCellStatus status,
                              std::uint32_t edgeTexels = 0,
                              std::uint32_t byteLength = 0) {
    CompositeChunkMsg message = {};
    message.cellX = cell.x;
    message.cellY = cell.y;
    message.edgeTexels = edgeTexels;
    message.byteLength = byteLength;
    message.status = status;
    return message;
}

}  // namespace

CompositeBatchStatus CompositeStreamerServer::streamNewlyVisible(
    const VisibleCellDelta& delta,
    IPC::Vec<CompositeChunkMsg>& out,
    IPC::Vec<std::uint8_t>& outBytes) const {
    out.clear();
    outBytes.clear();

    if (!pool_.active) {
        return CompositeBatchStatus::Inactive;
    }

    std::uint64_t streamedBytes = 0;
    for (const CellId& cell : delta) {
        const auto recordIt = pool_.byCell.find(cell);
        if (recordIt == pool_.byCell.end()) {
            if (!out.push_back(makeOutcome(cell, CompositeCellStatus::NotFound))) {
                return CompositeBatchStatus::OutputExhausted;
            }
            continue;
        }

        const CompositeRecord& record = recordIt->second;
        const std::uint64_t sliceEnd =
            record.poolOffset + static_cast<std::uint64_t>(record.byteLength);
        if (!validDxt1PayloadShape(record.edgeTexels, record.byteLength) ||
            sliceEnd < record.poolOffset ||
            sliceEnd > pool_.blob.size()) {
            if (!out.push_back(makeOutcome(
                    cell, CompositeCellStatus::Error, record.edgeTexels))) {
                return CompositeBatchStatus::OutputExhausted;
            }
            continue;
        }

        // The first valid payload is allowed through even when it exceeds the normal batch
        // budget, preventing a supported oversized cell from starving forever.
        if (streamedBytes != 0 &&
            streamedBytes + record.byteLength > kCompositeStreamBatchBytes) {
            if (!out.push_back(makeOutcome(
                    cell, CompositeCellStatus::Deferred, record.edgeTexels))) {
                return CompositeBatchStatus::OutputExhausted;
            }
            continue;
        }

        if (static_cast<std::uint64_t>(outBytes.size()) + record.byteLength >
            outBytes.max_size()) {
            if (!out.push_back(makeOutcome(
                    cell, CompositeCellStatus::Deferred, record.edgeTexels))) {
                return CompositeBatchStatus::OutputExhausted;
            }
            continue;
        }

        const std::uint32_t bytesBefore = outBytes.size();
        const std::uint8_t* source = pool_.blob.data() + record.poolOffset;
        bool payloadWritten = true;
        for (std::uint32_t i = 0; i < record.byteLength; ++i) {
            if (!outBytes.push_back(source[i])) {
                payloadWritten = false;
                break;
            }
        }

        if (!payloadWritten) {
            outBytes.truncate(bytesBefore);
            if (!out.push_back(makeOutcome(
                    cell, CompositeCellStatus::Error, record.edgeTexels))) {
                return CompositeBatchStatus::OutputExhausted;
            }
            continue;
        }

        const CompositeChunkMsg found = makeOutcome(
            cell, CompositeCellStatus::Found, record.edgeTexels, record.byteLength);
        if (!out.push_back(found)) {
            outBytes.truncate(bytesBefore);
            return CompositeBatchStatus::OutputExhausted;
        }
        streamedBytes += record.byteLength;
    }

    return CompositeBatchStatus::Complete;
}
