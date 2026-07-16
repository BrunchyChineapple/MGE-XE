#pragma once

#include <cstdint>

class ResidentCompositeCache;

namespace CompositeTelemetry {

// Per-frame event counters and current request-state gauges supplied by the reconcile loop.
struct FrameSample {
    std::uint32_t visibleCount = 0;
    std::uint32_t queuedCells = 0;
    std::uint32_t deferredBudget = 0;
    std::uint32_t deferredRetry = 0;
    std::uint32_t missingCached = 0;
    std::uint32_t pending = 0;

    std::uint32_t attemptedBatches = 0;
    std::uint32_t attemptedCells = 0;
    std::uint32_t issuedBatches = 0;
    std::uint32_t issuedCells = 0;
    std::uint32_t responseCells = 0;
    std::uint32_t admittedCells = 0;
    std::uint32_t budgetRejected = 0;
    std::uint32_t uploadFailed = 0;
    std::uint32_t notFoundResponses = 0;
    std::uint32_t deferredResponses = 0;
    std::uint32_t serverErrors = 0;
    std::uint32_t batchFailures = 0;
    std::uint32_t rpcFailures = 0;
    std::uint32_t rpcTimeouts = 0;
    std::uint32_t malformedResponses = 0;

    std::uint64_t responseBytes = 0;
    std::uint64_t admittedBytes = 0;
    std::uint64_t rpcWaitUs = 0;
    std::uint64_t reconcileUs = 0;
};

// Residency and request-state fields are current gauges. Every other numeric field is an
// interval total over sampleFrames and is emitted with a `_total` suffix.
struct Snapshot {
    std::uint32_t residentCount = 0;
    std::uint32_t residentBytes = 0;
    std::uint32_t atlasServed = 0;
    std::uint32_t visibleCount = 0;
    std::uint32_t queuedCells = 0;
    std::uint32_t deferredBudget = 0;
    std::uint32_t deferredRetry = 0;
    std::uint32_t missingCached = 0;
    std::uint32_t pending = 0;
    std::uint32_t sampleFrames = 0;

    std::uint64_t attemptedBatches = 0;
    std::uint64_t attemptedCells = 0;
    std::uint64_t issuedBatches = 0;
    std::uint64_t issuedCells = 0;
    std::uint64_t responseCells = 0;
    std::uint64_t responseBytes = 0;
    std::uint64_t admittedCells = 0;
    std::uint64_t admittedBytes = 0;
    std::uint64_t budgetRejected = 0;
    std::uint64_t uploadFailed = 0;
    std::uint64_t notFoundResponses = 0;
    std::uint64_t deferredResponses = 0;
    std::uint64_t serverErrors = 0;
    std::uint64_t batchFailures = 0;
    std::uint64_t rpcFailures = 0;
    std::uint64_t rpcTimeouts = 0;
    std::uint64_t malformedResponses = 0;
    std::uint64_t rpcWaitUs = 0;
    std::uint64_t reconcileUs = 0;
};

// Accumulates low-overhead 60-frame intervals. A visibility boundary flushes the old gauges
// and totals before the new frame is accumulated, so metrics from different sets never mix.
class Reporter {
public:
    void reset();
    void recordFrame(const ResidentCompositeCache& cache, const FrameSample& sample,
                     bool visibleSetChanged);

private:
    Snapshot snapshot_;
};

void write(const Snapshot& snapshot);

// Compact entry points retained for the format test and simple callers.
void write(const ResidentCompositeCache& cache);
void write(std::uint32_t residentCount, std::uint32_t residentBytes,
           std::uint32_t atlasServedThisFrame);

}  // namespace CompositeTelemetry
