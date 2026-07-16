#include "compositetelemetry.h"
#include "compositecache.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace CompositeTelemetry {

namespace {

constexpr std::uint64_t kBytesPerMB = 1024ull * 1024ull;
constexpr std::uint32_t kEmitIntervalFrames = 60;
constexpr const char* kMetricsFileName = "composite_metrics.txt";

bool g_pathResolved = false;
char g_metricsPath[MAX_PATH] = {};

void resolveMetricsPath() {
    if (g_pathResolved) {
        return;
    }

    char exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_pathResolved = true;
        return;
    }

    char* lastSep = strrchr(exePath, '\\');
    const size_t dirLen = lastSep ? static_cast<size_t>(lastSep - exePath + 1) : 0;
    char dir[MAX_PATH] = {};
    memcpy(dir, exePath, dirLen);
    _snprintf_s(g_metricsPath, sizeof(g_metricsPath), _TRUNCATE, "%s%s", dir,
                kMetricsFileName);
    g_pathResolved = true;
}

void writeBuffer(const char* buffer, int length) {
    resolveMetricsPath();
    if (g_metricsPath[0] == '\0' || length <= 0) {
        return;
    }

    HANDLE h = CreateFileA(g_metricsPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(h, buffer, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(h);
}

}  // namespace

void write(std::uint32_t residentCount, std::uint32_t residentBytes,
           std::uint32_t atlasServedThisFrame) {
    const double residentMB =
        static_cast<double>(residentBytes) / static_cast<double>(kBytesPerMB);

    char buffer[256] = {};
    const int length = _snprintf_s(
        buffer, sizeof(buffer), _TRUNCATE,
        "resident_count %u\nresident_bytes %u\nresident_mb %.3f\natlas_served %u\n",
        residentCount, residentBytes, residentMB, atlasServedThisFrame);
    writeBuffer(buffer, length);
}

void write(const ResidentCompositeCache& cache) {
    write(cache.residentCount(), cache.residentBytes(), cache.atlasServedThisFrame());
}

void write(const Snapshot& snapshot) {
    const double residentMB =
        static_cast<double>(snapshot.residentBytes) / static_cast<double>(kBytesPerMB);

    char buffer[1536] = {};
    const int length = _snprintf_s(
        buffer, sizeof(buffer), _TRUNCATE,
        "resident_count %u\n"
        "resident_bytes %u\n"
        "resident_mb %.3f\n"
        "atlas_served %u\n"
        "visible_count %u\n"
        "queued_cells %u\n"
        "deferred_budget %u\n"
        "deferred_retry %u\n"
        "missing_cached %u\n"
        "pending %u\n"
        "sample_frames_total %u\n"
        "attempted_batches_total %llu\n"
        "attempted_cells_total %llu\n"
        "issued_batches_total %llu\n"
        "issued_cells_total %llu\n"
        "response_cells_total %llu\n"
        "response_bytes_total %llu\n"
        "admitted_cells_total %llu\n"
        "admitted_bytes_total %llu\n"
        "budget_rejected_total %llu\n"
        "upload_failed_total %llu\n"
        "not_found_responses_total %llu\n"
        "deferred_responses_total %llu\n"
        "server_errors_total %llu\n"
        "batch_failures_total %llu\n"
        "rpc_failures_total %llu\n"
        "rpc_timeouts_total %llu\n"
        "malformed_responses_total %llu\n"
        "rpc_wait_us_total %llu\n"
        "reconcile_us_total %llu\n",
        snapshot.residentCount,
        snapshot.residentBytes,
        residentMB,
        snapshot.atlasServed,
        snapshot.visibleCount,
        snapshot.queuedCells,
        snapshot.deferredBudget,
        snapshot.deferredRetry,
        snapshot.missingCached,
        snapshot.pending,
        snapshot.sampleFrames,
        static_cast<unsigned long long>(snapshot.attemptedBatches),
        static_cast<unsigned long long>(snapshot.attemptedCells),
        static_cast<unsigned long long>(snapshot.issuedBatches),
        static_cast<unsigned long long>(snapshot.issuedCells),
        static_cast<unsigned long long>(snapshot.responseCells),
        static_cast<unsigned long long>(snapshot.responseBytes),
        static_cast<unsigned long long>(snapshot.admittedCells),
        static_cast<unsigned long long>(snapshot.admittedBytes),
        static_cast<unsigned long long>(snapshot.budgetRejected),
        static_cast<unsigned long long>(snapshot.uploadFailed),
        static_cast<unsigned long long>(snapshot.notFoundResponses),
        static_cast<unsigned long long>(snapshot.deferredResponses),
        static_cast<unsigned long long>(snapshot.serverErrors),
        static_cast<unsigned long long>(snapshot.batchFailures),
        static_cast<unsigned long long>(snapshot.rpcFailures),
        static_cast<unsigned long long>(snapshot.rpcTimeouts),
        static_cast<unsigned long long>(snapshot.malformedResponses),
        static_cast<unsigned long long>(snapshot.rpcWaitUs),
        static_cast<unsigned long long>(snapshot.reconcileUs));
    writeBuffer(buffer, length);
}

void Reporter::reset() {
    snapshot_ = Snapshot{};
}

void Reporter::recordFrame(const ResidentCompositeCache& cache,
                           const FrameSample& sample,
                           bool visibleSetChanged) {
    bool boundaryFlushed = false;
    if (visibleSetChanged && snapshot_.sampleFrames != 0) {
        write(snapshot_);
        snapshot_ = Snapshot{};
        boundaryFlushed = true;
    }

    snapshot_.residentCount = cache.residentCount();
    snapshot_.residentBytes = cache.residentBytes();
    snapshot_.atlasServed = cache.atlasServedThisFrame();
    snapshot_.visibleCount = sample.visibleCount;
    snapshot_.queuedCells = sample.queuedCells;
    snapshot_.deferredBudget = sample.deferredBudget;
    snapshot_.deferredRetry = sample.deferredRetry;
    snapshot_.missingCached = sample.missingCached;
    snapshot_.pending = sample.pending;
    ++snapshot_.sampleFrames;

    snapshot_.attemptedBatches += sample.attemptedBatches;
    snapshot_.attemptedCells += sample.attemptedCells;
    snapshot_.issuedBatches += sample.issuedBatches;
    snapshot_.issuedCells += sample.issuedCells;
    snapshot_.responseCells += sample.responseCells;
    snapshot_.responseBytes += sample.responseBytes;
    snapshot_.admittedCells += sample.admittedCells;
    snapshot_.admittedBytes += sample.admittedBytes;
    snapshot_.budgetRejected += sample.budgetRejected;
    snapshot_.uploadFailed += sample.uploadFailed;
    snapshot_.notFoundResponses += sample.notFoundResponses;
    snapshot_.deferredResponses += sample.deferredResponses;
    snapshot_.serverErrors += sample.serverErrors;
    snapshot_.batchFailures += sample.batchFailures;
    snapshot_.rpcFailures += sample.rpcFailures;
    snapshot_.rpcTimeouts += sample.rpcTimeouts;
    snapshot_.malformedResponses += sample.malformedResponses;
    snapshot_.rpcWaitUs += sample.rpcWaitUs;
    snapshot_.reconcileUs += sample.reconcileUs;

    const bool exceptionalOutcome = sample.budgetRejected != 0 ||
        sample.uploadFailed != 0 || sample.serverErrors != 0 ||
        sample.batchFailures != 0 || sample.rpcFailures != 0 ||
        sample.rpcTimeouts != 0 || sample.malformedResponses != 0;
    if (!boundaryFlushed &&
        (visibleSetChanged || exceptionalOutcome ||
         snapshot_.sampleFrames >= kEmitIntervalFrames)) {
        write(snapshot_);
        snapshot_ = Snapshot{};
    }
}

}  // namespace CompositeTelemetry
