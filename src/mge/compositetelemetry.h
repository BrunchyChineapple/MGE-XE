#pragma once

#include <cstdint>

// Composite_Telemetry (Architecture B, IPC client side / d3d8.dll, 32-bit).
//
// Reports the resident Per_Cell_Composite count (Req 8.1), the resident composite memory in
// megabytes (Req 8.2), and the number of cells served by the Single_Atlas_Path for the frame
// (Req 8.4), using the project's existing file-based metrics-sidecar pattern in the game root
// (Req 8.3). The sidecar is a direct analogue of rt_anticull.cpp's `rt_anticull_metrics.txt`
// and mega_geo_control.cpp's `mega_geo_metrics.txt`: the path is resolved once relative to the
// running executable (the directory the user drops d3d8.dll into, i.e. the game root), the file
// is rewritten in full on every emit (CREATE_ALWAYS), and the body is a tiny `key value` per
// line block written with a single _snprintf_s + WriteFile so a measurement harness (MWSE-Lua
// or external) can read it without any format dependency.
//
// File name (game root):  composite_metrics.txt
// Line format (one frame's snapshot, overwritten each emit):
//   resident_count <u32>\n
//   resident_bytes <u32>\n
//   resident_mb <float, bytes / 1 MiB>\n
//   atlas_served <u32>\n
//
// resident_mb is residentBytes converted to megabytes exactly as the authoritative model
// tests/composite_stream_model.py does (bytes_to_mb: byteCount / (1024*1024), i.e. 1 MiB).
//
// This is the clean seam task 11.2 calls from the client frame loop on Visible_Cell_Set
// changes. Two entry points are offered:
//   - write(const ResidentCompositeCache&)  pulls residentCount()/residentBytes()/
//                                            atlasServedThisFrame() off the cache (the common case)
//   - write(residentCount, residentBytes, atlasServedThisFrame)  takes the raw counts directly,
//                                            for callers that already have them in hand or for tests
// Neither call walks the scene, derives the visible set, or touches the cache state; they only
// read the supplied counts and append/rewrite the sidecar line. Both are no-ops if the sidecar
// path cannot be resolved, and never throw.

class ResidentCompositeCache;  // compositecache.h; reference-only here, included in the .cpp

namespace CompositeTelemetry {

// Emit one frame's Composite_Telemetry snapshot to the game-root sidecar, sourcing the counts
// from the ResidentCompositeCache accessors (Req 8.1/8.2/8.4). Call this when the
// Visible_Cell_Set changes (task 11.2).
void write(const ResidentCompositeCache& cache);

// Emit one frame's snapshot from raw counts. residentBytes is converted to megabytes for the
// resident_mb field (bytes / 1 MiB), matching the model. Used by callers that already hold the
// counts and by the sidecar-format unit test (task 11.4).
void write(std::uint32_t residentCount, std::uint32_t residentBytes,
           std::uint32_t atlasServedThisFrame);

}  // namespace CompositeTelemetry
