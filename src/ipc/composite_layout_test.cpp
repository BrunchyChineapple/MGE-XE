// composite_layout_test.cpp
//
// Task 5.3 — Unit test for struct field exposure (Requirement 5.3).
//
// This is a compile-time layout test: it includes the REAL runtime headers
// (mge/dlformat.h, which transitively pulls in ipc/bridge.h) and asserts, via
// static_assert + offsetof, that:
//
//   1. LandMesh exposes the four fields the Texture_Binder reads when selecting a
//      chunk's ground texture: compositeTex, cellX, cellY, cellValid (Req 5.3).
//   2. RenderMesh (the IPC-transferred visible-mesh record) exposes the same four
//      fields so the 32-bit client's binder can resolve per chunk.
//   3. The fields carry the binder-expected types (ptr32<IDirect3DTexture9> handle,
//      int32_t cell coordinates, bool validity).
//   4. The appended fields are append-only: every pre-existing field keeps the exact
//      offset it had before the append, and the new fields sit strictly AFTER the last
//      pre-existing field. This is what keeps the #pragma pack(4) layout agreement
//      between the 32-bit client (d3d8.dll, ptr32<T> == T*) and the 64-bit server
//      (mgeHost64.exe, MGE64_HOST -> ptr32<T> == uint32_t) intact.
//
// "Test passes" == "this translation unit compiles". The trivial main() lets it also
// be built and run as a standalone exe that returns 0. The offsets baked in below were
// verified empirically (patches/rtxdll/struct_offset_probe.cpp) to be identical in BOTH
// the x86-client and x64-host (MGE64_HOST) configurations, so the same assertions hold
// for whichever configuration compiles this file.
//
// Build (x86 client, ptr32<T> == T*):
//   cl /std:c++17 /EHsc /DWIN32 /DNOMINMAX /D_WINDOWS ^
//      /I MGE-XE\src /I "%DXSDK_DIR%\Include" composite_layout_test.cpp
//
// Build (x64 host, ptr32<T> == uint32_t):
//   cl /std:c++17 /EHsc /DWIN32 /DNOMINMAX /D_WINDOWS /DMGE64_HOST ^
//      /I MGE-XE\src /I "%DXSDK_DIR%\Include" composite_layout_test.cpp

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "mge/dlformat.h"   // LandMesh; transitively includes ipc/bridge.h (RenderMesh, CompositeChunkMsg)

// ---------------------------------------------------------------------------
// Pre-append baseline offsets (the "old" layout the binder must not disturb).
//
// These are the offsets of the fields that existed BEFORE task 5.1/5.2 appended the
// composite fields. Because C++ lays out members in declaration order with
// non-decreasing offsets, appending fields to the end of a struct cannot move any of
// these earlier members; asserting they still hold these values proves the append was
// purely additive. Values verified identical in x86-client and x64-host builds.
// ---------------------------------------------------------------------------

namespace baseline {
    // RenderMesh (pack(4)) pre-existing fields.
    constexpr std::size_t kRM_enabled   = 0;
    constexpr std::size_t kRM_hasAlpha  = 1;
    constexpr std::size_t kRM_animateUV = 2;
    constexpr std::size_t kRM_tex       = 4;    // ptr32, 4 bytes (4..7)
    constexpr std::size_t kRM_transform = 8;    // D3DXMATRIX, 64 bytes (8..71)
    constexpr std::size_t kRM_verts     = 72;   // int (72..75)
    constexpr std::size_t kRM_vBuffer   = 76;   // ptr32 (76..79)
    constexpr std::size_t kRM_faces     = 80;   // int (80..83)
    constexpr std::size_t kRM_iBuffer   = 84;   // ptr32 (84..87) -- LAST pre-existing field

    // LandMesh pre-existing fields.
    constexpr std::size_t kLM_sphere    = 0;    // BoundingSphere, 16 bytes (0..15)
    constexpr std::size_t kLM_box       = 16;   // BoundingBox, 48 bytes (16..63)
    constexpr std::size_t kLM_verts     = 64;   // DWORD (64..67)
    constexpr std::size_t kLM_faces     = 68;   // DWORD (68..71)
    constexpr std::size_t kLM_vbuffer   = 72;   // ptr32 (72..75)
    constexpr std::size_t kLM_ibuffer   = 76;   // ptr32 (76..79) -- LAST pre-existing field
}

// ===========================================================================
// RenderMesh — IPC-transferred visible-mesh record (ipc/bridge.h)
// ===========================================================================

// (A) The binder-read composite fields are EXPOSED on RenderMesh.
//     These references fail to compile if any field is missing or misnamed.
static_assert(sizeof(((RenderMesh*)nullptr)->compositeTex) > 0, "RenderMesh must expose compositeTex");
static_assert(sizeof(((RenderMesh*)nullptr)->cellX)        > 0, "RenderMesh must expose cellX");
static_assert(sizeof(((RenderMesh*)nullptr)->cellY)        > 0, "RenderMesh must expose cellY");
static_assert(sizeof(((RenderMesh*)nullptr)->cellValid)    > 0, "RenderMesh must expose cellValid");

// (B) ...with the types the Texture_Binder expects. compositeTex must be the same
//     per-cell composite handle type as the existing RenderMesh::tex (ptr32<IDirect3DTexture9>),
//     cell coordinates must be 32-bit signed, validity must be a bool.
static_assert(std::is_same_v<decltype(RenderMesh::compositeTex), decltype(RenderMesh::tex)>,
              "RenderMesh::compositeTex must match the existing tex handle type (ptr32<IDirect3DTexture9>)");
static_assert(std::is_same_v<decltype(RenderMesh::compositeTex), ptr32<IDirect3DTexture9>>,
              "RenderMesh::compositeTex must be ptr32<IDirect3DTexture9>");
static_assert(std::is_same_v<decltype(RenderMesh::cellX), int32_t>, "RenderMesh::cellX must be int32_t");
static_assert(std::is_same_v<decltype(RenderMesh::cellY), int32_t>, "RenderMesh::cellY must be int32_t");
static_assert(std::is_same_v<decltype(RenderMesh::cellValid), bool>, "RenderMesh::cellValid must be bool");

// (C) Pre-existing RenderMesh fields keep their exact offsets (append did not shift them).
static_assert(offsetof(RenderMesh, enabled)   == baseline::kRM_enabled,   "RenderMesh::enabled moved");
static_assert(offsetof(RenderMesh, hasAlpha)  == baseline::kRM_hasAlpha,  "RenderMesh::hasAlpha moved");
static_assert(offsetof(RenderMesh, animateUV) == baseline::kRM_animateUV, "RenderMesh::animateUV moved");
static_assert(offsetof(RenderMesh, tex)       == baseline::kRM_tex,       "RenderMesh::tex moved");
static_assert(offsetof(RenderMesh, transform) == baseline::kRM_transform, "RenderMesh::transform moved");
static_assert(offsetof(RenderMesh, verts)     == baseline::kRM_verts,     "RenderMesh::verts moved");
static_assert(offsetof(RenderMesh, vBuffer)   == baseline::kRM_vBuffer,   "RenderMesh::vBuffer moved");
static_assert(offsetof(RenderMesh, faces)     == baseline::kRM_faces,     "RenderMesh::faces moved");
static_assert(offsetof(RenderMesh, iBuffer)   == baseline::kRM_iBuffer,   "RenderMesh::iBuffer moved");

// (D) The new fields are appended STRICTLY AFTER the last pre-existing field (iBuffer),
//     i.e. they occupy fresh storage past the end of the old layout.
static_assert(offsetof(RenderMesh, compositeTex) > offsetof(RenderMesh, iBuffer),
              "RenderMesh::compositeTex must be appended after iBuffer");
static_assert(offsetof(RenderMesh, compositeTex)
                  >= baseline::kRM_iBuffer + sizeof(((RenderMesh*)nullptr)->iBuffer),
              "RenderMesh::compositeTex must start at/after the end of iBuffer's storage");

// (E) The new fields are in declaration order and the verified absolute offsets hold.
static_assert(offsetof(RenderMesh, compositeTex) == 88,  "RenderMesh::compositeTex offset drift");
static_assert(offsetof(RenderMesh, cellX)        == 92,  "RenderMesh::cellX offset drift");
static_assert(offsetof(RenderMesh, cellY)        == 96,  "RenderMesh::cellY offset drift");
static_assert(offsetof(RenderMesh, cellValid)    == 100, "RenderMesh::cellValid offset drift");
static_assert(offsetof(RenderMesh, compositeTex) < offsetof(RenderMesh, cellX), "compositeTex must precede cellX");
static_assert(offsetof(RenderMesh, cellX)        < offsetof(RenderMesh, cellY), "cellX must precede cellY");
static_assert(offsetof(RenderMesh, cellY)        < offsetof(RenderMesh, cellValid), "cellY must precede cellValid");

// (F) Total size is the pack(4) cross-process value (locks the whole layout).
static_assert(sizeof(RenderMesh) == 104, "RenderMesh size changed -- 32/64-bit IPC layout drift");

// ===========================================================================
// LandMesh — runtime distant-land chunk struct (mge/dlformat.h)
// ===========================================================================

// (A) The binder-read composite fields are EXPOSED on LandMesh.
static_assert(sizeof(((LandMesh*)nullptr)->compositeTex) > 0, "LandMesh must expose compositeTex");
static_assert(sizeof(((LandMesh*)nullptr)->cellX)        > 0, "LandMesh must expose cellX");
static_assert(sizeof(((LandMesh*)nullptr)->cellY)        > 0, "LandMesh must expose cellY");
static_assert(sizeof(((LandMesh*)nullptr)->cellValid)    > 0, "LandMesh must expose cellValid");

// (B) ...with the binder-expected types, mirroring the DistantSubset::tex handle pattern.
static_assert(std::is_same_v<decltype(LandMesh::compositeTex), ptr32<IDirect3DTexture9>>,
              "LandMesh::compositeTex must be ptr32<IDirect3DTexture9>");
static_assert(std::is_same_v<decltype(LandMesh::compositeTex), decltype(DistantSubset::tex)>,
              "LandMesh::compositeTex must match the DistantSubset::tex handle type");
static_assert(std::is_same_v<decltype(LandMesh::cellX), int32_t>, "LandMesh::cellX must be int32_t");
static_assert(std::is_same_v<decltype(LandMesh::cellY), int32_t>, "LandMesh::cellY must be int32_t");
static_assert(std::is_same_v<decltype(LandMesh::cellValid), bool>, "LandMesh::cellValid must be bool");

// (C) Pre-existing LandMesh fields keep their exact offsets (append did not shift them).
static_assert(offsetof(LandMesh, sphere)  == baseline::kLM_sphere,  "LandMesh::sphere moved");
static_assert(offsetof(LandMesh, box)     == baseline::kLM_box,     "LandMesh::box moved");
static_assert(offsetof(LandMesh, verts)   == baseline::kLM_verts,   "LandMesh::verts moved");
static_assert(offsetof(LandMesh, faces)   == baseline::kLM_faces,   "LandMesh::faces moved");
static_assert(offsetof(LandMesh, vbuffer) == baseline::kLM_vbuffer, "LandMesh::vbuffer moved");
static_assert(offsetof(LandMesh, ibuffer) == baseline::kLM_ibuffer, "LandMesh::ibuffer moved");

// (D) The new fields are appended STRICTLY AFTER the last pre-existing field (ibuffer).
static_assert(offsetof(LandMesh, compositeTex) > offsetof(LandMesh, ibuffer),
              "LandMesh::compositeTex must be appended after ibuffer");
static_assert(offsetof(LandMesh, compositeTex)
                  >= baseline::kLM_ibuffer + sizeof(((LandMesh*)nullptr)->ibuffer),
              "LandMesh::compositeTex must start at/after the end of ibuffer's storage");

// (E) The new fields are in declaration order and the verified absolute offsets hold.
static_assert(offsetof(LandMesh, compositeTex) == 80, "LandMesh::compositeTex offset drift");
static_assert(offsetof(LandMesh, cellX)        == 84, "LandMesh::cellX offset drift");
static_assert(offsetof(LandMesh, cellY)        == 88, "LandMesh::cellY offset drift");
static_assert(offsetof(LandMesh, cellValid)    == 92, "LandMesh::cellValid offset drift");
static_assert(offsetof(LandMesh, compositeTex) < offsetof(LandMesh, cellX), "compositeTex must precede cellX");
static_assert(offsetof(LandMesh, cellX)        < offsetof(LandMesh, cellY), "cellX must precede cellY");
static_assert(offsetof(LandMesh, cellY)        < offsetof(LandMesh, cellValid), "cellY must precede cellValid");

// ===========================================================================
// CompositeChunkMsg — the per-cell stream record the binder fields are fed from.
// Locking its layout here guards the same 32/64-bit pack(4) agreement.
// ===========================================================================
static_assert(std::is_same_v<decltype(CompositeChunkMsg::cellX), int32_t>, "CompositeChunkMsg::cellX must be int32_t");
static_assert(std::is_same_v<decltype(CompositeChunkMsg::cellY), int32_t>, "CompositeChunkMsg::cellY must be int32_t");
static_assert(offsetof(CompositeChunkMsg, cellX)      == 0,  "CompositeChunkMsg::cellX moved");
static_assert(offsetof(CompositeChunkMsg, cellY)      == 4,  "CompositeChunkMsg::cellY moved");
static_assert(offsetof(CompositeChunkMsg, edgeTexels) == 8,  "CompositeChunkMsg::edgeTexels moved");
static_assert(offsetof(CompositeChunkMsg, byteLength) == 12, "CompositeChunkMsg::byteLength moved");
static_assert(sizeof(CompositeChunkMsg) == 16, "CompositeChunkMsg size changed -- IPC layout drift");

int main() {
    // All meaningful checks are compile-time static_asserts above. Reaching here means
    // the struct field-exposure and append-only-offset contract for LandMesh/RenderMesh
    // holds for the configuration that compiled this translation unit.
    return 0;
}
