#pragma once

#include "proxydx/d3d9header.h"
#include "mge/compositecache.h"

// Texture_Binder helper (design "Texture_Binder"). Selects the stage-0 ground texture
// for one distant-land chunk: the chunk's resident Per_Cell_Composite when available and
// within budget, otherwise the global world.dds atlas. The result is NEVER null, so an
// unresolved/over-budget cell always renders through the Single_Atlas_Path (Req 5.1, 5.2,
// 6.1). The cache lookup returns null for a non-resident or budget-rejected cell.
//
// chunk carries the additive RenderMesh/LandMesh fields: compositeTex (a resident handle
// already on the mesh, or null), cellValid, and (cellX, cellY).
template <typename ChunkT>
static inline IDirect3DTexture9* selectGroundTexture(
    const ChunkT& chunk,
    const ResidentCompositeCache& cache,
    IDirect3DTexture9* atlas /* texWorldColour */) {
    IDirect3DTexture9* t = chunk.compositeTex ? (IDirect3DTexture9*)chunk.compositeTex : nullptr;
    if (!t && chunk.cellValid) {
        t = cache.lookup(CellId{ chunk.cellX, chunk.cellY });
    }
    return t ? t : atlas;
}
