#pragma once

#define WIN32_LEAN_AND_MEAN
#include <cstddef>   // offsetof, for the DXCompressedLandVertex layout static_asserts below
#include "../3rdparty/niflib/include/nif_math.h"

struct DXVertex {
    Niflib::Vector3 Position;
    Niflib::Vector3 Normal;
    unsigned char Diffuse[4];
    Niflib::TexCoord texCoord;
};

struct DXCompressedVertex {
    unsigned short Position[4];
    unsigned char Normal[4];
    unsigned char Diffuse[4];
    unsigned short texCoord[2];
};

struct DXCompressedLandVertex {
    Niflib::Vector3 Position;   // 12 bytes @0  world position (unchanged offset)
    short texCoord[2];          //  4 bytes @12 SHORT2N province-atlas UV (unchanged offset)
    unsigned char Normal[4];    //  4 bytes @16 xyz surface normal UBYTE4N (decode (b/255)*2-1), w pad
};
// Total 20 bytes. This on-disk layout is the cross-process contract shared by both the 32-bit
// client (d3d8.dll) and the 64-bit IPC server (mgeHost64.exe); SIZEOFLANDVERT in distantshader.h
// must equal sizeof(DXCompressedLandVertex). The Normal field is appended at the tail so Position
// (offset 0) and texCoord (offset 12) keep their existing offsets and the shader-path LandElem
// declaration only needs one new element at offset 16.

// Compile-time guarantees for the land-vertex layout (task 6.3; Req 8.1, 8.3, 8.4). These live in
// the header so every translation unit that includes DXVertex.h -- on both the 32-bit and 64-bit
// builds -- inherits the same checks. Any accidental change to field order, type, padding, or size
// fails the build here instead of silently misaligning the runtime vertex read in another process.
// DXCompressedLandVertex is standard-layout (Niflib::Vector3 is a plain public float x/y/z struct
// with no virtuals or bases), so offsetof is well-defined on every member.
static_assert(sizeof(DXCompressedLandVertex) == 20u,
              "DXCompressedLandVertex must be 20 bytes (Position 12 + texCoord 4 + Normal 4)");
static_assert(offsetof(DXCompressedLandVertex, Position) == 0u,
              "DXCompressedLandVertex::Position must be at offset 0");
static_assert(offsetof(DXCompressedLandVertex, texCoord) == 12u,
              "DXCompressedLandVertex::texCoord must be at offset 12");
static_assert(offsetof(DXCompressedLandVertex, Normal) == 16u,
              "DXCompressedLandVertex::Normal must be at offset 16");
