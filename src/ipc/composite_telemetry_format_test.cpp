// composite_telemetry_format_test.cpp
//
// Task 11.4 - Unit test for the Composite_Telemetry sidecar line format (Requirement 8.3).
//
// Req 8.3: "THE Composite_Telemetry SHALL write its output using the project's existing
// file-based metrics-sidecar pattern in the game root."
//
// This test EXERCISES THE REAL WRITER. It compiles and links the production
// mge/compositetelemetry.cpp and calls CompositeTelemetry::write(residentCount,
// residentBytes, atlasServedThisFrame) - the raw-count entry point the header explicitly
// designates "for ... the sidecar-format unit test (task 11.4)". Because the real writer
// runs, the assertions cover the actual _snprintf_s format string, field order, %u/%.3f
// conversions, the resident_mb = bytes / 1 MiB unit conversion, and the real
// GetModuleFileNameA path-resolution that lands the file in the game root.
//
// What is CONCRETELY EXERCISED (the real writer runs and we read its output):
//   (1) Line format / field order / units. After write(...), the produced file is read back
//       and compared byte-for-byte to the expected 4-line `key value` block. Hardcoded
//       literals lock the field NAMES and ORDER (resident_count, resident_bytes, resident_mb,
//       atlas_served); a parallel re-format of the documented format string covers arbitrary
//       fractional-MiB inputs so the units/precision are checked independently of the chosen
//       constants. This is the same `key value`-per-line metrics-sidecar shape that
//       rt_anticull.cpp (rt_anticull_metrics.txt) and mega_geo_control.cpp
//       (mega_geo_metrics.txt) emit (single _snprintf_s + WriteFile, full rewrite each emit).
//   (2) resident_mb = residentBytes / 1 MiB. Exercised with an exact-MiB value (3 MiB ->
//       "3.000"), a half-MiB value (1.5 MiB -> "1.500"), and a rounding case
//       (700000 bytes -> 0.66757... -> "%.3f" -> "0.668").
//   (3) Full rewrite each emit (CREATE_ALWAYS). A large emit followed by a small emit must
//       leave the file equal to ONLY the small emit (no leftover bytes), proving the truncate.
//   (4) The file lands in the GAME ROOT (the running exe's directory), NOT the current working
//       directory. The test sets the cwd to a different temp directory BEFORE the first emit
//       (path resolution is cached on first write), then asserts the sidecar appears next to
//       the exe (resolved here with the SAME GetModuleFileNameA -> strip-to-dir idiom the
//       writer and rt_anticull/mega_geo use) and is ABSENT from the temp cwd.
//
// ASSERTED-BY-CONSTRUCTION (not separately re-tested here):
//   - CompositeTelemetry::write(const ResidentCompositeCache&) is a thin forwarder that reads
//     cache.residentCount()/residentBytes()/atlasServedThisFrame() and calls the raw overload
//     exercised above, so it emits byte-identical lines. We link it (providing stub accessor
//     definitions below so the link resolves) but drive the format through the raw overload,
//     which is the documented seam for this test.
//
// Build (x86 client config; telemetry is IPC-client / d3d8.dll, 32-bit): see
// patches/rtxdll/build_telemetry_format_test.bat. "Test passes" == process returns 0 and
// prints ALL_TELEMETRY_FORMAT_CHECKS_PASS.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "mge/compositetelemetry.h"   // CompositeTelemetry::write(...) - the REAL writer under test
#include "mge/compositecache.h"       // ResidentCompositeCache (for the link stubs below)

// ---------------------------------------------------------------------------
// Link stubs.
//
// compositetelemetry.cpp also defines write(const ResidentCompositeCache&), which references
// three ResidentCompositeCache accessors. Those live in compositecache.cpp, which we do NOT
// link (it pulls in the whole D3D9 client). We never construct a cache here; these stub
// definitions exist only so the unused-by-this-test forwarder links cleanly regardless of
// whether the linker discards it. They are never called.
// ---------------------------------------------------------------------------
std::uint32_t ResidentCompositeCache::residentCount() const { return 0; }
std::uint32_t ResidentCompositeCache::residentBytes() const { return 0; }
std::uint32_t ResidentCompositeCache::atlasServedThisFrame() const { return 0; }

namespace {

constexpr const char* kMetricsFileName = "composite_metrics.txt";
constexpr std::uint64_t kBytesPerMB = 1024ull * 1024ull;  // MiB, matches the writer

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  PASS  %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

// Read an entire file into a std::string of raw bytes. Returns false if it cannot be opened.
bool readWholeFile(const char* path, std::string& out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    out.clear();
    char buf[512];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return true;
}

bool fileExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolve the directory of the running executable (the game root), using the SAME
// GetModuleFileNameA -> strrchr('\\') -> truncate idiom that CompositeTelemetry,
// rt_anticull, and mega_geo_control use. Returned string ends with a trailing backslash.
std::string exeDir() {
    char exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::string();
    }
    char* lastSep = strrchr(exePath, '\\');
    const size_t dirLen = lastSep ? static_cast<size_t>(lastSep - exePath + 1) : 0;
    return std::string(exePath, dirLen);
}

// Re-format the documented sidecar block independently, so fractional-MiB cases are checked
// against the format applied with the same MiB constant rather than a hand-rounded literal.
std::string expectedBlock(std::uint32_t residentCount, std::uint32_t residentBytes,
                          std::uint32_t atlasServed) {
    const double residentMB = static_cast<double>(residentBytes) / static_cast<double>(kBytesPerMB);
    char buf[256] = {};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "resident_count %u\nresident_bytes %u\nresident_mb %.3f\natlas_served %u\n",
        residentCount, residentBytes, residentMB, atlasServed);
    return std::string(buf);
}

}  // namespace

int main() {
    std::printf("=== Composite_Telemetry sidecar format test (task 11.4, Req 8.3) ===\n");

    const std::string dir = exeDir();
    if (dir.empty()) {
        std::printf("  FAIL  could not resolve exe dir\n");
        return 1;
    }
    const std::string sidecarInGameRoot = dir + kMetricsFileName;

    // Put the process cwd somewhere OTHER than the exe dir so the "lands in game root, not cwd"
    // assertion is meaningful. The writer caches its resolved path on the first write(), so the
    // cwd must already differ before that first call.
    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    char cwdProbe[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwdProbe);
    SetCurrentDirectoryA(tempDir);
    std::string sidecarInCwd = std::string(tempDir);
    if (!sidecarInCwd.empty() && sidecarInCwd.back() != '\\') sidecarInCwd += '\\';
    sidecarInCwd += kMetricsFileName;

    // Clean slate.
    DeleteFileA(sidecarInGameRoot.c_str());
    DeleteFileA(sidecarInCwd.c_str());

    // ---- Emit A: exact 3 MiB, drives the real writer ---------------------------------------
    const std::uint32_t aCount = 7u, aBytes = 3u * 1024u * 1024u /* 3 MiB */, aAtlas = 5u;
    CompositeTelemetry::write(aCount, aBytes, aAtlas);

    // (4) File landed in the game root (exe dir), not the cwd.
    check(fileExists(sidecarInGameRoot.c_str()),
          "sidecar written to the game root (running-exe directory)");
    check(!fileExists(sidecarInCwd.c_str()),
          "sidecar NOT written to the current working directory (resolved via exe path, not cwd)");

    // (1) Exact field names + order + units, locked against a hardcoded literal.
    {
        std::string got;
        const bool read = readWholeFile(sidecarInGameRoot.c_str(), got);
        check(read, "sidecar is readable after emit A");
        const std::string expectLiteral =
            "resident_count 7\nresident_bytes 3145728\nresident_mb 3.000\natlas_served 5\n";
        check(got == expectLiteral,
              "emit A matches the exact 4-line `key value` block (field names, order, %u, %.3f)");
        // Cross-check against the independently re-formatted block.
        check(got == expectedBlock(aCount, aBytes, aAtlas),
              "emit A matches the documented format string applied to its inputs");
        // Field order is line 0..3 = resident_count, resident_bytes, resident_mb, atlas_served.
        check(got.rfind("resident_count ", 0) == 0,
              "line 0 is resident_count (matches metrics-sidecar `key value` shape)");
    }

    // ---- (3) Full rewrite each emit: a small emit B must fully replace large emit A --------
    const std::uint32_t bCount = 0u, bBytes = 0u, bAtlas = 0u;
    CompositeTelemetry::write(bCount, bBytes, bAtlas);
    {
        std::string got;
        readWholeFile(sidecarInGameRoot.c_str(), got);
        const std::string expectB =
            "resident_count 0\nresident_bytes 0\nresident_mb 0.000\natlas_served 0\n";
        check(got == expectB,
              "emit B fully rewrites the file (CREATE_ALWAYS truncate, no leftover bytes from A)");
    }

    // ---- (2) resident_mb = bytes / 1 MiB: half-MiB and a rounding case ---------------------
    {
        const std::uint32_t halfBytes = 1572864u;  // 1.5 MiB exactly
        CompositeTelemetry::write(2u, halfBytes, 1u);
        std::string got;
        readWholeFile(sidecarInGameRoot.c_str(), got);
        check(got.find("resident_mb 1.500\n") != std::string::npos,
              "resident_mb renders 1572864 bytes as 1.500 (bytes / 1 MiB)");
        check(got == expectedBlock(2u, halfBytes, 1u),
              "half-MiB emit matches the documented format applied to its inputs");
    }
    {
        const std::uint32_t oddBytes = 700000u;  // 700000 / 1048576 = 0.66757... -> %.3f -> 0.668
        CompositeTelemetry::write(1u, oddBytes, 0u);
        std::string got;
        readWholeFile(sidecarInGameRoot.c_str(), got);
        check(got.find("resident_mb 0.668\n") != std::string::npos,
              "resident_mb rounds 700000 bytes to 0.668 (%.3f of bytes / 1 MiB)");
        check(got == expectedBlock(1u, oddBytes, 0u),
              "rounding emit matches the documented format applied to its inputs");
    }

    // ---- Cleanup: remove the test artifact; restore cwd ------------------------------------
    DeleteFileA(sidecarInGameRoot.c_str());
    DeleteFileA(sidecarInCwd.c_str());
    SetCurrentDirectoryA(cwdProbe);

    if (g_failures == 0) {
        std::printf("ALL_TELEMETRY_FORMAT_CHECKS_PASS\n");
        return 0;
    }
    std::printf("TELEMETRY_FORMAT_CHECKS_FAILED count=%d\n", g_failures);
    return 1;
}
