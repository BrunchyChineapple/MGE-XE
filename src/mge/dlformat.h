#pragma once

#include "ipc/bridge.h"
#include "dlmath.h"
#include <vector>

enum StaticType {
    STATIC_AUTO = 0,
    STATIC_NEAR = 1,
    STATIC_FAR = 2,
    STATIC_VERY_FAR = 3,
    STATIC_GRASS = 4,
    STATIC_TREE = 5,
    STATIC_BUILDING = 6
};

struct LandMesh {
    BoundingSphere sphere;
    BoundingBox box;
    DWORD verts;
    DWORD faces;
    ptr32<IDirect3DVertexBuffer9> vbuffer;
    ptr32<IDirect3DIndexBuffer9> ibuffer;
    // NEW (additive; zero-initialized for Old_Format so the binder falls back).
    // Mirrors the DistantSubset::tex pattern; appended at the end of the struct so
    // prior field offsets are unchanged. LandMesh instances are value-initialized
    // (vector::resize in distantinit.cpp initLandscape), which zeroes these fields,
    // so an Old_Format load leaves compositeTex null and cellValid false -> fallback.
    ptr32<IDirect3DTexture9>      compositeTex;  // resident per-cell composite, or null
    int32_t                       cellX, cellY;  // cell identity for cache lookup
    bool                          cellValid;     // false on Old_Format
};

#pragma pack(push, 4)
struct DistantSubset {
    BoundingSphere sphere;
    D3DXVECTOR3 aabbMin, aabbMax;       // corners of the axis-aligned bounding box
    ptr32<IDirect3DTexture9> tex;
    bool hasAlpha, hasUVController;
    ptr32<IDirect3DVertexBuffer9> vbuffer;
    ptr32<IDirect3DIndexBuffer9> ibuffer;
    int verts;
    int faces;
};

struct DistantStatic {
    unsigned char type;
    BoundingSphere sphere;
    D3DXVECTOR3 aabbMin, aabbMax;       // corners of the axis-aligned bounding box
    DWORD firstSubsetIndex;
    DWORD numSubsets;
};
#pragma pack(pop)

struct UsedDistantStatic {
    DWORD staticRef;
    uint16_t visIndex;
    D3DXVECTOR3 pos;
    float scale;
    D3DXMATRIX transform;
    BoundingSphere sphere;      // post-transform
    BoundingBox box;            // post-transform

    BoundingSphere GetBoundingSphere(const BoundingSphere& base) const {
        BoundingSphere sphere;
        D3DXVec3TransformCoord(&sphere.center, &base.center, &transform);
        sphere.radius = base.radius * scale;

        return sphere;
    }

    BoundingBox GetBoundingBox(const D3DXVECTOR3& aabbMin, const D3DXVECTOR3& aabbMax) const {
        BoundingBox box;
        box.Set(aabbMin, aabbMax);
        box.Transform(transform);

        return box;
    }
};