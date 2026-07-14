
#include "compositetelemetry.h"
#include "compositecache.h"   // ResidentCompositeCache accessors
#include "support/log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

// Composite_Telemetry sidecar writer. A direct port of rt_anticull.cpp's / mega_geo_control.cpp's
// metrics-sidecar I/O: the path is resolved once relative to the running exe (the game root the
// user drops d3d8.dll into), the file is rewritten in full on every emit (CREATE_ALWAYS), and the
// body is a tiny `key value` per line block written with one _snprintf_s + WriteFile. See
// compositetelemetry.h for the contract and the exact line format.

namespace CompositeTelemetry {

namespace {

// One megabyte, binary (MiB). resident_mb = residentBytes / 1 MiB, matching
// tests/composite_stream_model.py::bytes_to_mb (byteCount / (1024*1024)).
constexpr std::uint64_t kBytesPerMB = 1024ull * 1024ull;

// Sidecar lives next to the running exe (the game root), same directory rt_anticull's
// `rt_anticull_metrics.txt` and mega_geo's `mega_geo_metrics.txt` land in.
constexpr const char* kMetricsFileName = "composite_metrics.txt";

bool g_pathResolved = false;
char g_metricsPath[MAX_PATH] = {};

// Resolve the metrics-sidecar path next to the running executable. Done once; cached in
// g_metricsPath. Mirrors rt_anticull::resolveConfigPath / mega_geo_control::resolvePaths
// (GetModuleFileNameA -> strip to dir -> append the file name). On failure g_metricsPath is left
// empty and every write() becomes a no-op.
void resolveMetricsPath() {
    if (g_pathResolved) {
        return;
    }
    char exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_pathResolved = true;  // give up; leave g_metricsPath empty
        return;
    }
    char* lastSep = strrchr(exePath, '\\');
    const size_t dirLen = lastSep ? static_cast<size_t>(lastSep - exePath + 1) : 0;
    char dir[MAX_PATH] = {};
    memcpy(dir, exePath, dirLen);
    _snprintf_s(g_metricsPath, sizeof(g_metricsPath), _TRUNCATE, "%s%s", dir, kMetricsFileName);
    g_pathResolved = true;
}

}  // namespace

void write(std::uint32_t residentCount, std::uint32_t residentBytes,
           std::uint32_t atlasServedThisFrame) {
    resolveMetricsPath();
    if (g_metricsPath[0] == '\0') {
        return;
    }

    HANDLE h = CreateFileA(g_metricsPath, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    // resident_mb = residentBytes / 1 MiB (Req 8.2), the bytes_to_mb conversion from the model.
    const double residentMB = static_cast<double>(residentBytes) / static_cast<double>(kBytesPerMB);

    char buf[256] = {};
    int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "resident_count %u\nresident_bytes %u\nresident_mb %.3f\natlas_served %u\n",
        residentCount, residentBytes, residentMB, atlasServedThisFrame);
    if (len > 0) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr);
    }
    CloseHandle(h);
}

void write(const ResidentCompositeCache& cache) {
    // Source the counts straight from the cache accessors (Req 8.1/8.2/8.4) and forward to the
    // raw-count writer so both entry points emit byte-identical sidecar lines.
    write(cache.residentCount(), cache.residentBytes(), cache.atlasServedThisFrame());
}

}  // namespace CompositeTelemetry
