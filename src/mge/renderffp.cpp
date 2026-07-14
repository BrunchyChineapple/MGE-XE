/**
 * renderffp.cpp — Fixed-function pipeline rendering for RTX Remix
 *
 * Provides alternative rendering paths for distant land that bypass D3DX
 * effect shaders and use the D3D9 fixed-function pipeline instead.
 * Remix understands fixed-function perfectly and can extract proper
 * textures, transforms, and materials from it.
 */

#ifdef MGE_RTX

#include "distantland.h"
#include "distantshader.h"
#include "configuration.h"
#include "mwbridge.h"
#include "proxydx/d3d8header.h"
#include "support/log.h"
#include "mge/compositebinder.h"   // selectGroundTexture (Texture_Binder, task 9.5)

// --- Direct Remix-API batched-submission experiment (distant statics) ---
#ifndef REMIX_ALLOW_X86
#define REMIX_ALLOW_X86
#endif
#include "remix_api_test.h"   // RemixAPITest::getInterface()
#include "remix_c.h"          // remixapi_* structs / entrypoints
#include "world_batch.h"      // worldBatchTier1Enabled() — runtime in-game toggle (MGE Batching MCM)
#include "haze_config.h"      // hazeConfig() — live MCM/env tuning for the aerial-perspective haze
#include "dlcull_config.h"    // dlCullConfig() — live MCM/env tuning for the distant-static near-cull
#include <cstdlib>            // getenv
#include <cstdio>             // swprintf_s (texture-hash pseudo-path formatting)
#include <cmath>              // powf/expf/exp2f/sqrtf/fabsf (aerial-perspective inscatter bake)

// FVF for uncompressed statics: position + normal + diffuse color + texcoord
#define STATIC_FFP_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct StaticFFPVertex {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes)
    DWORD color;          // Diffuse color (4 bytes)
    float u, v;           // Texcoord (8 bytes)
};                        // Total: 36 bytes

static const int SIZEOF_STATIC_FFP_VERT = sizeof(StaticFFPVertex);

// FVF for uncompressed land: position + normal + ONE texcoord.
// Remix's FFP geometry path only ray-traces single-texcoord-set vertices: a TEX2 (dual-UV) land
// vertex draws successfully at the D3D9 level (HRESULT 0) but Remix silently produces no geometry,
// so the distant land vanished. The rejection was specific to a second TEXCOORD set, NOT to a
// normal: the distant-statics FFP vertex (StaticFFPVertex) is XYZ|NORMAL|DIFFUSE|TEX1 and Remix
// ray-traces it correctly. We therefore keep a SINGLE texcoord set (and write the PER-CELL composite
// UV into it, each land mesh being exactly one cell) while adding D3DFVF_NORMAL so Remix shades the
// terrain from smooth per-vertex normals instead of faceted geometry-derived ones. The detail stage
// tiles off the same single texcoord set. The atlas fallback (rare, transient: a cell newly entering
// view before its composite has streamed) samples world.dds with this per-cell UV; it self-corrects
// within a frame once resident.
#define LAND_FFP_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

struct LandFFPVertex {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes) — smooth per-vertex normal decoded from the land vertex
    float u, v;          // Per-cell composite texcoord, [0,1] within the cell (8 bytes)
};                       // Total: 32 bytes

static const int SIZEOF_LAND_FFP_VERT = sizeof(LandFFPVertex);

// Maps from compressed VB → uncompressed FFP VB
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> staticFFPBuffers;
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> landFFPBuffers;

static float halfToFloat(unsigned short h) {
    unsigned int sign = (h >> 15) & 1;
    unsigned int exponent = (h >> 10) & 0x1F;
    unsigned int mantissa = h & 0x3FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            unsigned int result = sign << 31;
            return *(float*)&result;
        } else {
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                exponent--;
            }
            exponent++;
            mantissa &= ~0x400;
        }
    } else if (exponent == 31) {
        unsigned int result = (sign << 31) | 0x7F800000 | (mantissa << 13);
        return *(float*)&result;
    }

    exponent = exponent + (127 - 15);
    mantissa = mantissa << 13;
    unsigned int result = (sign << 31) | (exponent << 23) | mantissa;
    return *(float*)&result;
}

static IDirect3DVertexBuffer9* getOrCreateStaticFFPBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* compressedVB, int vertCount) {
    if (!compressedVB || vertCount <= 0) return nullptr;

    auto it = staticFFPBuffers.find(compressedVB);
    if (it != staticFFPBuffers.end()) {
        return it->second;
    }

    IDirect3DVertexBuffer9* ffpVB = nullptr;
    HRESULT hr = device->CreateVertexBuffer(vertCount * SIZEOF_STATIC_FFP_VERT, D3DUSAGE_WRITEONLY, STATIC_FFP_FVF, D3DPOOL_DEFAULT, &ffpVB, nullptr);
    if (FAILED(hr)) {
        LOG::logline("!! Failed to create FFP vertex buffer for statics (verts=%d, size=%d, hr=0x%08X)", vertCount, vertCount * SIZEOF_STATIC_FFP_VERT, hr);
        return nullptr;
    }

    void* srcData = nullptr;
    void* dstData = nullptr;
    compressedVB->Lock(0, 0, &srcData, D3DLOCK_READONLY);
    ffpVB->Lock(0, 0, &dstData, 0);

    if (srcData && dstData) {
        const unsigned char* src = (const unsigned char*)srcData;
        StaticFFPVertex* dst = (StaticFFPVertex*)dstData;

        for (int i = 0; i < vertCount; i++) {
            const unsigned short* pos16 = (const unsigned short*)(src + 0);
            dst->x = halfToFloat(pos16[0]);
            dst->y = halfToFloat(pos16[1]);
            dst->z = halfToFloat(pos16[2]);

            const unsigned char* norm = src + 8;
            dst->nx = (norm[0] / 255.0f) * 2.0f - 1.0f;
            dst->ny = (norm[1] / 255.0f) * 2.0f - 1.0f;
            dst->nz = (norm[2] / 255.0f) * 2.0f - 1.0f;

            dst->color = *(const DWORD*)(src + 12);

            const unsigned short* tc16 = (const unsigned short*)(src + 16);
            dst->u = halfToFloat(tc16[0]);
            dst->v = halfToFloat(tc16[1]);

            src += 20; // SIZEOFSTATICVERT
            dst++;
        }
    }

    compressedVB->Unlock();
    ffpVB->Unlock();

    staticFFPBuffers[compressedVB] = ffpVB;
    return ffpVB;
}

static IDirect3DVertexBuffer9* getOrCreateLandFFPBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* compressedVB, int vertCount, int cellX, int cellY) {
    if (!compressedVB || vertCount <= 0) return nullptr;

    auto it = landFFPBuffers.find(compressedVB);
    if (it != landFFPBuffers.end()) {
        return it->second;
    }

    IDirect3DVertexBuffer9* ffpVB = nullptr;
    HRESULT hr = device->CreateVertexBuffer(vertCount * SIZEOF_LAND_FFP_VERT, D3DUSAGE_WRITEONLY, LAND_FFP_FVF, D3DPOOL_DEFAULT, &ffpVB, nullptr);
    if (FAILED(hr)) {
        LOG::logline("!! Failed to create FFP vertex buffer for land (verts=%d, size=%d, hr=0x%08X)", vertCount, vertCount * SIZEOF_LAND_FFP_VERT, hr);
        return nullptr;
    }

    // Cell origin in world units. Each land mesh is exactly one 8192-unit cell now, so per-cell
    // composite UVs are u = frac((worldX - cellMinX)/8192), v = 1 - frac((worldY - cellMinY)/8192)
    // (the V flip matches the top-left texture origin the Composite_Baker renders into). Computing
    // the UV here from the vertex's own world XY keeps the on-disk world format unchanged.
    const float cellMinX = (float)cellX * 8192.0f;
    const float cellMinY = (float)cellY * 8192.0f;

    void* srcData = nullptr;
    void* dstData = nullptr;
    compressedVB->Lock(0, 0, &srcData, D3DLOCK_READONLY);
    ffpVB->Lock(0, 0, &dstData, 0);

    if (srcData && dstData) {
        const unsigned char* src = (const unsigned char*)srcData;
        LandFFPVertex* dst = (LandFFPVertex*)dstData;

        for (int i = 0; i < vertCount; i++) {
            const float* pos = (const float*)(src + 0);
            dst->x = pos[0];
            dst->y = pos[1];
            dst->z = pos[2];

            // Appended UBYTE4N surface normal at offset 16 within the 20-byte compressed land vertex
            // (Position@0, texCoord@12, Normal@16). Decode xyz exactly like the proven statics path:
            // (b/255)*2-1. The 4th byte is pad and ignored. Supplying this normal lets Remix shade the
            // terrain smoothly even though the FFP land pass runs D3DRS_LIGHTING FALSE.
            const unsigned char* norm = src + 16;
            dst->nx = (norm[0] / 255.0f) * 2.0f - 1.0f;
            dst->ny = (norm[1] / 255.0f) * 2.0f - 1.0f;
            dst->nz = (norm[2] / 255.0f) * 2.0f - 1.0f;

            // Per-cell composite UV from world position into the single texcoord set:
            // u = frac((worldX - cellMinX)/8192), v = 1 - frac((worldY - cellMinY)/8192). The V flip
            // matches the top-left texture origin the Composite_Baker renders into. Clamped via floor
            // so a vertex on the far cell seam (frac == 0) maps to the edge instead of wrapping.
            float cu = (pos[0] - cellMinX) / 8192.0f;
            float cv = (pos[1] - cellMinY) / 8192.0f;
            cu -= floorf(cu);
            cv -= floorf(cv);
            dst->u = cu;
            dst->v = 1.0f - cv;

            src += SIZEOFLANDVERT; // 20: Position(12) + texCoord(4) + Normal(4)
            dst++;
        }
    }

    compressedVB->Unlock();
    ffpVB->Unlock();

    landFFPBuffers[compressedVB] = ffpVB;
    return ffpVB;
}


// ---------------------------------------------------------------------------
// Direct Remix-API batched submission of distant statics (Tier 1).
//
// Transcodes the SAME geometry renderDistantStaticsFFP already draws via FFP into the
// fork's CreateMeshBatched + DrawInstance path: the mesh is the LOCAL decoded geometry and
// the real per-instance world matrix rides on the DrawInstance (mirroring the working FFP
// path, NOT MegaGeo's world-space identity bake). info.hash == the legacy replacement key so
// existing toolkit replacements apply. Gated by worldBatchTier1Enabled() (the MGE Batching
// MCM master + "distant" sub-toggle; falls back to the mge_batch_statics.txt marker / env
// MGE_BATCH_STATICS when no MCM cfg is present). See world_batch.h.


static std::unordered_map<IDirect3DVertexBuffer9*, remixapi_MeshHandle> g_batchMeshCache;

// Per-VB frame number when first submitted this session. Used by the FFP-suppression
// warm-up gate: a static is drawn additively (FFP + batched) on its first frame so the
// FFP draw registers its texture (the fork legacy-texture registry is populated from the
// D3D9 draw path) and the API material finalizes; from the NEXT frame on, the duplicate
// FFP draw is suppressed and the batched copy stands alone. Cleared in releaseFFPBuffers.
static std::unordered_map<IDirect3DVertexBuffer9*, uint32_t> g_batchVBFirstFrame;
static uint32_t g_batchFrameCounter = 0;

// FNV-1a 64. Used to give each batched mesh a STABLE, content-derived hash (not the
// per-session VB pointer) so the geometry has a consistent identity across runs and can
// be targeted/replaced in the Remix toolkit like a normal captured asset.
static uint64_t fnv1a64(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Tier 1 materials: one Remix material per source static texture. Keyed by the
// bound IDirect3DTexture9* (distant statics are 1 VB : 1 texture, so this is 1:1
// with g_batchMeshCache in practice). The material binds the captured VANILLA
// texture directly via the fork's "0x<imagehash>" albedoTexture pseudo-path
// (rtx_fork_api_entry.cpp::textureHashPathLookup resolves it against the live
// TextureManager table by getImageHash()), so the batched copy shows the real
// texture WITHOUT needing a USD replacement or an on-disk file path. info.hash is
// also set to the texture hash so that, when a USD material replacement IS keyed
// to this texture (E-man water, retextures), Remix's getReplacementMaterial(hash)
// still wins and the replacement is inherited. A cached null means hash lookup
// failed (e.g. sysmem texture) — those fall back to the Remix default material.
static std::unordered_map<IDirect3DTexture9*, remixapi_MaterialHandle> g_batchMaterialCache;

static remixapi_MaterialHandle ensureBatchMaterial(remixapi_Interface* remix,
                                                   IDirect3DTexture9* tex, bool hasAlpha) {
    auto it = g_batchMaterialCache.find(tex);
    if (it != g_batchMaterialCache.end()) return it->second;

    remixapi_MaterialHandle h = nullptr;
    uint64_t texHash = 0;
    if (tex && remix->CreateMaterial && remix->dxvk_GetTextureHash &&
        remix->dxvk_GetTextureHash(tex, &texHash) == REMIXAPI_ERROR_CODE_SUCCESS && texHash != 0) {

        // Fork pseudo-path: "0x<hex>" -> captured texture with that image hash.
        wchar_t albedoPath[24];
        swprintf_s(albedoPath, L"0x%016llX", (unsigned long long)texHash);

        remixapi_MaterialInfoOpaqueEXT op = {};
        op.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        op.albedoConstant.x = 1.0f; op.albedoConstant.y = 1.0f; op.albedoConstant.z = 1.0f;
        op.opacityConstant = 1.0f;
        op.roughnessConstant = 1.0f;
        op.metallicConstant = 0.0f;
        // Mirror the FFP alpha behaviour: cutout statics (foliage) use GREATEREQUAL
        // @ ref 128 (AlphaTestType::kGreaterOrEqual = 6); opaque statics DISABLE the
        // alpha test, which maps to kAlways = 7 (always pass). NOTE: 0 is kNever
        // (discard every fragment) — using it for opaque made textured meshes
        // invisible once a real albedo texture was bound (the alpha test activates).
        op.alphaTestType = hasAlpha ? 6 : 7;
        op.alphaReferenceValue = 128;

        remixapi_MaterialInfo mi = {};
        mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        mi.pNext = &op;
        mi.hash = texHash;              // == texture hash -> inherit USD replacements
        mi.albedoTexture = albedoPath;  // captured vanilla texture by image hash
        mi.emissiveIntensity = 0.0f;
        // Sampler: match the game's linear+wrap state. Defaults (0) are
        // lss::Mdl::Filter::Nearest + WrapMode::Clamp, which caused point-sampling
        // banding and far statics collapsing to one color (tiled UVs clamping to a
        // single edge texel). Linear (1) + Repeat (1) mirror the FFP sampler.
        mi.filterMode = 1;   // lss::Mdl::Filter::Linear
        mi.wrapModeU  = 1;   // lss::Mdl::WrapMode::Repeat
        mi.wrapModeV  = 1;   // lss::Mdl::WrapMode::Repeat

        remixapi_ErrorCode rc = remix->CreateMaterial(&mi, &h);
        if (rc != REMIXAPI_ERROR_CODE_SUCCESS) h = nullptr;
    }

    g_batchMaterialCache[tex] = h;  // cache even null to avoid re-attempting
    return h;
}

static remixapi_MeshHandle createBatchedStaticMesh(remixapi_Interface* remix, const RenderMesh& mesh) {
    if (!mesh.vBuffer || !mesh.iBuffer || mesh.verts <= 0 || mesh.faces <= 0) return nullptr;

    // Decode LOCAL vertices from the compressed static VB: half3 pos @0,
    // ubyte4 normal @8, D3DCOLOR @12, half2 uv @16, stride 20 (same as getOrCreateStaticFFPBuffer).
    std::vector<remixapi_HardcodedVertex> verts(mesh.verts);
    void* vsrc = nullptr;
    if (FAILED(mesh.vBuffer->Lock(0, 0, &vsrc, D3DLOCK_READONLY)) || !vsrc) return nullptr;
    {
        const unsigned char* p = (const unsigned char*)vsrc;
        for (int i = 0; i < mesh.verts; ++i) {
            remixapi_HardcodedVertex hv = {};
            const unsigned short* pos16 = (const unsigned short*)(p + 0);
            hv.position[0] = halfToFloat(pos16[0]);
            hv.position[1] = halfToFloat(pos16[1]);
            hv.position[2] = halfToFloat(pos16[2]);
            const unsigned char* n = p + 8;
            hv.normal[0] = (n[0] / 255.0f) * 2.0f - 1.0f;
            hv.normal[1] = (n[1] / 255.0f) * 2.0f - 1.0f;
            hv.normal[2] = (n[2] / 255.0f) * 2.0f - 1.0f;
            hv.color = *(const DWORD*)(p + 12);
            const unsigned short* tc = (const unsigned short*)(p + 16);
            hv.texcoord[0] = halfToFloat(tc[0]);
            hv.texcoord[1] = halfToFloat(tc[1]);
            verts[i] = hv;
            p += 20;
        }
    }
    mesh.vBuffer->Unlock();

    // Indices: triangle list, mesh.faces tris, 0-based into the VB (the FFP draw
    // uses BaseVertexIndex 0 / StartIndex 0).
    const uint32_t indexCount = (uint32_t)mesh.faces * 3u;
    std::vector<uint32_t> idx(indexCount);
    D3DINDEXBUFFER_DESC id = {};
    mesh.iBuffer->GetDesc(&id);
    void* isrc = nullptr;
    if (FAILED(mesh.iBuffer->Lock(0, 0, &isrc, D3DLOCK_READONLY)) || !isrc) return nullptr;
    if (id.Format == D3DFMT_INDEX32) {
        const uint32_t* s = (const uint32_t*)isrc;
        for (uint32_t k = 0; k < indexCount; ++k) idx[k] = s[k];
    } else {
        const unsigned short* s = (const unsigned short*)isrc;
        for (uint32_t k = 0; k < indexCount; ++k) idx[k] = s[k];
    }
    mesh.iBuffer->Unlock();

    remixapi_MeshInfoSurfaceTriangles surf = {};
    surf.vertices_values = verts.data();
    surf.vertices_count  = verts.size();
    surf.indices_values  = idx.data();
    surf.indices_count   = idx.size();
    surf.skinning_hasvalue = 0;
    // Bake the captured-vanilla-texture material (or default on lookup failure)
    // into the surface. Statics are 1 VB : 1 texture, so the per-VB mesh cache and
    // per-texture material cache stay consistent.
    surf.material = ensureBatchMaterial(remix, mesh.tex, mesh.hasAlpha);

    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    // Stable content hash (vertices + indices), NOT the VB pointer: gives the batched
    // mesh a consistent identity across runs so it captures + replaces in the toolkit
    // like a normal asset. verts are zero-initialized (HardcodedVertex has explicit
    // padding) so the byte hash is deterministic.
    uint64_t meshHash = fnv1a64(verts.data(), verts.size() * sizeof(remixapi_HardcodedVertex));
    meshHash = fnv1a64(idx.data(), idx.size() * sizeof(uint32_t), meshHash);
    info.hash  = meshHash;
    info.surfaces_values = &surf;
    info.surfaces_count  = 1;

    remixapi_MeshHandle h = nullptr;
    remixapi_ErrorCode rc = remix->CreateMeshBatched(&info, &h);
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? h : nullptr;
}

// Submit a distant static through the Remix API. Returns TRUE when the duplicate FFP
// draw should be SUPPRESSED (the batched copy fully stands in for it), FALSE when the
// caller must still run the FFP draw (warm-up frame, or batching unavailable for this
// mesh so the FFP draw is the correct visual).
//
// CRITICAL ordering: the batched mesh+material are NOT created until the VB has been
// seen on a PRIOR frame. The "0x<hash>" material albedo resolves against the fork
// legacy-texture registry, which is populated by the FFP draw (D3D9Rtx::processTextures).
// Since this submit runs BEFORE the FFP draw in the loop (so it can request suppression
// via an early continue), creating the material on the first frame would finalize it
// before that frame's FFP draw registered the texture -> white material cached forever.
// Deferring creation to the next frame guarantees the prior frame's FFP draw already
// registered the texture (commands are processed FIFO), so the albedo resolves.
static bool submitStaticBatched(remixapi_Interface* remix, const RenderMesh& mesh, uint32_t frame) {
    // Warm-up gate FIRST — before creating anything.
    auto fit = g_batchVBFirstFrame.find(mesh.vBuffer);
    if (fit == g_batchVBFirstFrame.end()) {
        g_batchVBFirstFrame[mesh.vBuffer] = frame;   // first sighting
        return false;                                 // -> FFP draws, registers the texture
    }
    if (frame <= fit->second) {
        return false;                                 // same first frame (other instances) -> FFP draws
    }

    // frame > firstFrame: the texture is registered now. Create the mesh+material once.
    remixapi_MeshHandle h = nullptr;
    auto it = g_batchMeshCache.find(mesh.vBuffer);
    if (it != g_batchMeshCache.end()) {
        h = it->second;
    } else {
        h = createBatchedStaticMesh(remix, mesh);   // also bakes/caches the texture-resolved material
        g_batchMeshCache[mesh.vBuffer] = h;          // cache even null to avoid re-attempting a bad VB
    }

    // Need BOTH a valid mesh and a valid (texture-resolved) material to stand in for
    // the FFP draw. If either is missing, submit NOTHING and let the FFP draw render
    // (a flat/default batched copy coincident with the FFP geometry would z-fight).
    remixapi_MaterialHandle mat = nullptr;
    auto mit = g_batchMaterialCache.find(mesh.tex);
    if (mit != g_batchMaterialCache.end()) mat = mit->second;
    if (!h || !mat) return false;

    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.categoryFlags = 0;
    info.mesh = h;
    // D3DXMATRIX is row-vector (translation in _41.._43). remix_Transform is a
    // row-major 3x4 used column-vector (M*v), so it's the transpose of D3D's upper
    // 3 rows, with translation in matrix[r][3]. Real position now (Z-lift removed).
    const D3DXMATRIX& m = mesh.transform;
    info.transform.matrix[0][0] = m._11; info.transform.matrix[0][1] = m._21; info.transform.matrix[0][2] = m._31; info.transform.matrix[0][3] = m._41;
    info.transform.matrix[1][0] = m._12; info.transform.matrix[1][1] = m._22; info.transform.matrix[1][2] = m._32; info.transform.matrix[1][3] = m._42;
    info.transform.matrix[2][0] = m._13; info.transform.matrix[2][1] = m._23; info.transform.matrix[2][2] = m._33; info.transform.matrix[2][3] = m._43;
    info.doubleSided = 1;
    remix->DrawInstance(&info);
    return true;   // established -> suppress the duplicate FFP draw
}

// Render distant statics using fixed-function pipeline
#ifdef MGE_RTX
// Squared radial cull distance for distant statics, from the live "MGE Distant Cull"
// MCM (mge_dlcull.cfg, polled ~500ms) with MGE_DL_NEARCULL_CELLS env fallback. Returns
// 0 when disabled. See dlcull_config.h for the why. Kills the "double trees" by skipping
// MGE distant statics the engine is still drawing as near statics.
static float dlNearCullDistSq() {
    const DLCullConfig& c = dlCullConfig();
    if (!c.enabled || c.cells <= 0.0f) {
        return 0.0f;
    }
    const float d = c.cells * DistantLand::kCellSize;
    return d * d;
}
#endif

void DistantLand::renderDistantStaticsFFP() {
    if (!MWBridge::get()->IsExterior()) {
        float clipAt = nearViewRange - 768.0f;
        D3DXPLANE clipPlane(0, 0, clipAt, -(mwProj._33 * clipAt + mwProj._43));
        device->SetClipPlane(0, clipPlane);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
    }

    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(STATIC_FFP_FVF);

    device->SetTransform(D3DTS_VIEW, &mwView);
    D3DXMATRIX distProj = mwProj;
    editProjectionZ(&distProj, kDistantNearPlane, Configuration.DL.DrawDist * kCellSize);
    device->SetTransform(D3DTS_PROJECTION, &distProj);

    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);

    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&kDistantZBias);

    // Render each visible static using the VisibleSet iterator API
    IDirect3DTexture9* lastTex = nullptr;
    IDirect3DVertexBuffer9* lastVB = nullptr;

    // Batched-submission experiment: resolve the Remix interface once per frame.
    remixapi_Interface* batchRemix = nullptr;
    if (worldBatchTier1Enabled() && RemixAPITest::isInitialized()) {
        batchRemix = RemixAPITest::getInterface();
        if (batchRemix && (!batchRemix->CreateMeshBatched || !batchRemix->DrawInstance || !batchRemix->CreateMaterial)) {
            batchRemix = nullptr;
        }
    }

    // Give external (API-submitted) geometry a camera with the EXTENDED distant far
    // plane. Confirmed root cause of the "submits rc=0 but invisible" bug: external
    // DrawInstance geometry is projected with getCamera(Main)'s projection, which is
    // Morrowind's vanilla SHORT far plane, so distant batched statics clip out. The
    // FFP distant draw only renders because it uses the extended distProj. SetupCamera
    // -> processExternalCamera overrides the World/Main camera matrices used by our
    // external draws. Basis from the view matrix columns (D3D LH lookAt), fov/aspect
    // from the projection, far = the distant draw distance.
    if (batchRemix && batchRemix->SetupCamera) {
        remixapi_CameraInfoParameterizedEXT pc = {};
        pc.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
        pc.position.x = eyePos.x; pc.position.y = eyePos.y; pc.position.z = eyePos.z;
        pc.right.x   = mwView._11; pc.right.y   = mwView._21; pc.right.z   = mwView._31;
        pc.up.x      = mwView._12; pc.up.y      = mwView._22; pc.up.z      = mwView._32;
        pc.forward.x = mwView._13; pc.forward.y = mwView._23; pc.forward.z = mwView._33;
        pc.fovYInDegrees = atanf(1.0f / mwProj._22) * 2.0f * 57.2957795131f;
        pc.aspect    = mwProj._22 / mwProj._11;
        pc.nearPlane = kDistantNearPlane;
        pc.farPlane  = Configuration.DL.DrawDist * kCellSize;
        remixapi_CameraInfo ci = {};
        ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
        ci.pNext = &pc;
        ci.type  = REMIXAPI_CAMERA_TYPE_WORLD;
        batchRemix->SetupCamera(&ci);
    }

    // One batch frame number per distant-statics pass; drives the warm-up gate.
    uint32_t batchFrame = batchRemix ? ++g_batchFrameCounter : 0u;

    auto drawStatics = [&](auto& vset) {
        // Reusable scratch buffer: retains capacity across frames so the per-frame
        // drain of the (already state-sorted) visible set costs no heap alloc/free.
        // Render is single-threaded; one static per lambda instantiation (shared vs
        // non-shared), only one of which runs per config.
        static std::vector<RenderMesh> localMeshes;
        localMeshes.clear();
        vset.Reset();
#ifdef MGE_RTX
        const float nearCullSq = dlNearCullDistSq();  // 0 => disabled
#endif
        while (!vset.AtEnd()) {
            RenderMesh m = vset.Next();
            if (!m.vBuffer || m.verts <= 0) continue;
#ifdef MGE_RTX
            // Scoped per-tree suppress: drop the distant copy of any object whose source
            // texture is on the MCM suppress list (i.e. the trees you've replaced in Remix),
            // so only your replacement renders. Everything else keeps its distant static.
            if (dlIsDistantTextureSuppressed(m.tex)) continue;
            // Blanket radial near-cull the engine is already drawing near (see dlNearCullDistSq).
            if (nearCullSq > 0.0f) {
                const float dx = m.transform._41 - eyePos.x;
                const float dy = m.transform._42 - eyePos.y;
                if (dx * dx + dy * dy < nearCullSq) continue;
            }
#endif
            localMeshes.push_back(m);
        }

        // One-entry cache: the statics set is state-sorted (sortVisibleSet ByState),
        // so identical source VBs are adjacent. Reusing the last resolved FFP VB skips
        // the unordered_map lookup for every mesh in a run after the first.
        IDirect3DVertexBuffer9* lastCompressedVB = nullptr;
        IDirect3DVertexBuffer9* lastResolvedFFP = nullptr;

        for (const auto& mesh : localMeshes) {

            // Batched submission first. When it fully stands in for the FFP draw
            // (warmed up + valid mesh & texture-resolved material), suppress ALL the
            // duplicate FFP work for this mesh — the actual perf win: no per-mesh
            // SetTexture / SetTransform / FFP-VB resolve / SetStreamSource /
            // DrawIndexedPrimitive, and no per-frame Remix hashing of the FFP draw.
            if (batchRemix && submitStaticBatched(batchRemix, mesh, batchFrame)) {
                continue;
            }

            if (mesh.tex != lastTex) {
                device->SetTexture(0, mesh.tex);
                device->SetRenderState(D3DRS_ALPHATESTENABLE, mesh.hasAlpha);
                lastTex = mesh.tex;
            }

            device->SetTransform(D3DTS_WORLD, &mesh.transform);

            IDirect3DVertexBuffer9* ffpVB;
            if (mesh.vBuffer == lastCompressedVB) {
                ffpVB = lastResolvedFFP;
            } else {
                ffpVB = getOrCreateStaticFFPBuffer(device, mesh.vBuffer, mesh.verts);
                lastCompressedVB = mesh.vBuffer;
                lastResolvedFFP = ffpVB;
            }
            if (!ffpVB) continue;

            if (ffpVB != lastVB) {
                device->SetStreamSource(0, ffpVB, 0, SIZEOF_STATIC_FFP_VERT);
                device->SetIndices(mesh.iBuffer);
                lastVB = ffpVB;
            }

            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.verts, 0, mesh.faces);
        }
    };

    if (Configuration.UseSharedMemory) {
        drawStatics(visDistantShared);
    } else {
        drawStatics(visDistant);
    }

    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

// Render distant land terrain using fixed-function pipeline
void DistantLand::renderDistantLandFFP() {
    if (!MWBridge::get()->IsExterior()) return;

    D3DXMATRIX distProj = mwProj;
    editProjectionZ(&distProj, kDistantNearPlane, Configuration.DL.DrawDist * kCellSize);
    D3DXVECTOR4 viewsphere(eyePos.x, eyePos.y, eyePos.z, Configuration.DL.DrawDist * kCellSize);

    // Cull
    D3DXMATRIX viewproj = mwView * distProj;
    ViewFrustum frustum(&viewproj);

    if (Configuration.UseSharedMemory) {
        visLandShared.RemoveAll();
        ipcClient.getVisibleMeshes(visLandSharedId, frustum, viewsphere, VIS_LAND);
        ipcClient.waitForCompletion();
    } else {
        visLand.RemoveAll();
        DistantLandShare::LandQuadTree.GetVisibleMeshes(frustum, viewsphere, visLand);
        visLand.SortByState();
        visLand.RetainWithTTL();
        visLand.SortByState();
    }

    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(LAND_FFP_FVF);

    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);
    // Offset distant land downward to prevent overlap with MW's near terrain
    identity._43 = -40.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &mwView);
    device->SetTransform(D3DTS_PROJECTION, &distProj);

    // Texture_Binder (task 9.5): the stage-0 ground texture is now chosen PER CHUNK
    // inside the drawLand loop below (selectGroundTexture -> per-cell composite or the
    // world.dds atlas), so the single pre-loop device->SetTexture(0, texWorldColour) is
    // gone. The stage-0 sampler state still applies to whatever texture each chunk binds.
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    // --- RTX aerial-perspective inscatter (Route 1: per-cell TFACTOR + D3DTOP_ADD) ---
    // Reproduce MGE's per-vertex fogColour() inscatter (XE Common.fx) on the CPU and feed it as
    // D3DRS_TEXTUREFACTOR into a D3DTOP_ADD stage so Remix ray-traces albedo = groundTexture + inscatter
    // per cell. Self-gates via niceWeather (clear/cloudy) + distance; terrain-only so sky/sun are never
    // fogged. Exact math + uniform mappings: patches/rtxdll/findings_mgexe_inscatter_bake_math.md.
    // Additive-only approximation of fogApply (f.a*terrain + f.rgb): we bake f.rgb, drop the f.a
    // extinction (single captured stage cannot multiply AND add).
    // NOTE: was gated on (Configuration.MGEFlags & USE_ATM_SCATTER); the math self-gates via
    // niceWeather + distance, so we drive it from the live MCM/env config instead.
    // Live tuning via the "MGE Haze" MCM (mge_haze.cfg, polled ~500ms) with MGE_HAZE_* env fallback.
    // Knobs: enabled, start (0 => nearViewRange), range, strength, desat. See haze_config.h.
    const HazeConfig& hz = hazeConfig();
    const bool aerialHaze = hz.enabled;

    auto sat01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    const bool  isExpFog  = (Configuration.MGEFlags & EXP_FOG) != 0;
    const RGBVECTOR skyColU = MWBridge::get()->CellHasWeather()
        ? *MWBridge::get()->getCurrentWeatherSkyCol() : horizonCol;
    const float nsx = skyColU.r + (atmSkylightScatter.x - skyColU.r) * atmSkylightScatter.w;   // shader newSkyCol
    const float nsy = skyColU.g + (atmSkylightScatter.y - skyColU.g) * atmSkylightScatter.w;
    const float nsz = skyColU.b + (atmSkylightScatter.z - skyColU.b) * atmSkylightScatter.w;
    const float sAlt   = powf(1.0f + sunPos.z, 10.0f);
    const float sAlt_a = 2.8f + 4.3f / sAlt;
    const float sAlt_b = sat01(1.0f - exp2f(-1.9f * sAlt));
    const float sAlt_c = sat01(expf(-4.0f * sunPos.z)) * sat01(sAlt);

    // Evaluate MGE fogColour().rgb at a cell-center point. Prototype uses eye height (horizontal ray);
    // Route 3 will evaluate per-vertex with true altitude. Returns the additive inscatter as D3DCOLOR.
    auto inscatterTFactor = [&](float cx, float cy) -> D3DCOLOR {
        const float vx = cx - eyePos.x, vy = cy - eyePos.y;             // P.z == eyePos.z -> vz = 0
        const float dist = sqrtf(vx * vx + vy * vy);
        if (dist < 1e-3f) return 0;
        const float dx = vx / dist, dy = vy / dist;                    // dz = 0
        // Haze ramp uses our own start/range (NOT MGE's far weather fog), so it covers the whole
        // distant-land band. hStart defaults to nearViewRange (distant-land begin).
        const float hStart = (hz.start > 0.0f) ? hz.start : nearViewRange;
        float fogdist = (dist - hStart) / hz.range;
        const float fog = sat01(expf(-fogdist));                       // extinction ramp (drives storm skyDir)
        fogdist = sat01(0.224f * fogdist);
        float r = horizonCol.r * (1.0f - fog);                         // skyColDirectional *= (1 - fog)
        float g = horizonCol.g * (1.0f - fog);
        float b = horizonCol.b * (1.0f - fog);
        if (niceWeather > 0.001f && eyePos.z > -1.0f) {
            const float suncos = dx * sunPos.x + dy * sunPos.y;        // dz*sunPos.z == 0
            const float mie  = (1.58f / (1.24f - suncos)) * sAlt_c;
            const float rayl = 1.0f - 0.09f * mie;
            const float atmdep = 1.33f;                                // 1.33 * exp(-2*saturate(dz=0)) = 1.33
            const float t = 0.5f * (1.0f + suncos);
            const float ssx = atmInscatter.r + (atmOutscatter.r - atmInscatter.r) * t;
            const float ssy = atmInscatter.g + (atmOutscatter.g - atmInscatter.g) * t;
            const float ssz = atmInscatter.b + (atmOutscatter.b - atmInscatter.b) * t;
            const float amul = atmdep * (sAlt_a + 0.7f * mie);
            auto attF = [&](float a) { return (fabsf(a) > 1e-6f) ? (1.0f - expf(-fogdist * a)) / a : fogdist; };
            const float ax = attF(ssx * amul), ay = attF(ssy * amul), az = attF(ssz * amul);
            const float k = (1.17f * atmdep + 0.89f) * sAlt_b;
            const float c0 = (0.125f * mie + nsx * rayl) * ax * k;
            const float c1 = (0.125f * mie + nsy * rayl) * ay * k;
            const float c2 = (0.125f * mie + nsz * rayl) * az * k;
            r += (c0 - r) * niceWeather;                               // lerp(skyDir, color, niceWeather)
            g += (c1 - g) * niceWeather;
            b += (c2 - b) * niceWeather;
        }
        // Desaturate toward neutral luminance to tame the saturated clear-day Rayleigh blue.
        const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        r += (lum - r) * hz.desat;
        g += (lum - g) * hz.desat;
        b += (lum - b) * hz.desat;
        const int ir = (int)(sat01(r * hz.strength) * 255.0f + 0.5f);
        const int ig = (int)(sat01(g * hz.strength) * 255.0f + 0.5f);
        const int ib = (int)(sat01(b * hz.strength) * 255.0f + 0.5f);
        return D3DCOLOR_XRGB(ir, ig, ib);
    };

    device->SetRenderState(D3DRS_LIGHTING, FALSE);

    if (aerialHaze) {
        // albedo = groundTexture + TFACTOR(inscatter). Stage 1 detail is raster-only (Remix captures
        // only stage 0's op); single-stage TFactor => Remix's isTextureFactorBlend stays false (no extra multiply).
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    } else {
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    // RTX detail-texture stopgap: the shader path (XE Mod Landscape.fx) modulates
    // the low-res baked world atlas with the tiling world_detail.dds to inject
    // high-frequency surface break-up (`result *= detail`, detail centred on 1.0).
    // The FFP/Remix path historically bound only the atlas (single SELECTARG1
    // stage), leaving distant ground visibly smeared. Re-add the detail as a
    // second stage that MODULATE2X-blends the tiling detail over the base. The
    // detail texture averages ~mid-grey, so MODULATE2X (x2) is brightness-neutral,
    // matching the shader's multiply-about-1.0. UVs reuse the single atlas UV set
    // (texcoord index 0) scaled up by a texture-transform matrix so the detail
    // repeats many times per cell instead of stretching across the whole world.
    if (texWorldDetail) {
        // Tiling factor on the atlas UV ([0,1] across the whole province). Mirrors
        // the finer of the shader's two octaves (texcoord * 333) at a calmer value
        // so distant mips don't dissolve to flat grey. Single tunable knob.
        static const float kDetailTile = 160.0f;
        D3DXMATRIX mDetail;
        D3DXMatrixIdentity(&mDetail);
        mDetail._11 = kDetailTile;
        mDetail._22 = kDetailTile;
        device->SetTransform(D3DTS_TEXTURE1, &mDetail);

        device->SetTexture(1, texWorldDetail);
        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    } else {
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }

    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Render each visible land chunk
    IDirect3DVertexBuffer9* lastVB = nullptr;

    auto drawLand = [&](auto& vset) {
        // Reusable scratch buffer (see drawStatics): no per-frame heap alloc/free.
        static std::vector<RenderMesh> localMeshes;
        localMeshes.clear();
        vset.Reset();
        while (!vset.AtEnd()) {
            RenderMesh m = vset.Next();
            if (m.vBuffer && m.verts > 0) {
                localMeshes.push_back(m);
            }
        }

        for (const auto& mesh : localMeshes) {

            IDirect3DVertexBuffer9* ffpVB = getOrCreateLandFFPBuffer(device, mesh.vBuffer, mesh.verts, mesh.cellX, mesh.cellY);
            if (!ffpVB) continue;

            // Texture_Binder (task 9.5): pick this chunk's stage-0 ground texture — the
            // chunk's resident Per_Cell_Composite when available and within budget, else the
            // global world.dds atlas (texWorldColour). selectGroundTexture never returns null
            // (Req 5.1, 5.2, 6.1). Exactly ONE stage-0 texture is bound per chunk, matching the
            // Remix_FFP_Constraint that Remix ray-traces the FFP path from a single bound
            // texture per draw — no live multi-texture splat (Req 5.5).
            IDirect3DTexture9* groundTex = selectGroundTexture(mesh, DistantLand::compositeCache, texWorldColour);

            // Single texcoord set (per-cell UV) drives whatever is bound to stage 0, so no texcoord
            // index switching is needed. Both the composite and the (transient) atlas fallback sample
            // with the per-cell UV; once a cell's composite is resident the bind is correct.

            // Per-chunk fallback (Req 6.4): if binding the composite fails, fall back to the
            // atlas for THIS chunk only; if the atlas bind also fails, skip ONLY this chunk and
            // keep rendering the rest of the distant land. A texture failure on one chunk never
            // breaks the loop, so a single transient failure can't blank the whole terrain.
            HRESULT hrTex = device->SetTexture(0, groundTex);
            if (FAILED(hrTex) && groundTex != texWorldColour) {
                hrTex = device->SetTexture(0, texWorldColour);
            }
            if (FAILED(hrTex)) {
                continue;
            }

            if (ffpVB != lastVB) {
                device->SetStreamSource(0, ffpVB, 0, SIZEOF_LAND_FFP_VERT);
                device->SetIndices(mesh.iBuffer);
                lastVB = ffpVB;
            }

            // Per-cell aerial-perspective inscatter -> TFACTOR for the D3DTOP_ADD stage (Route 1).
            // cellValid is false on Old_Format (no per-cell coords) -> black tfactor = ADD is a no-op.
            if (aerialHaze) {
                const D3DCOLOR tf = mesh.cellValid
                    ? inscatterTFactor((float)mesh.cellX * 8192.0f + 4096.0f,
                                       (float)mesh.cellY * 8192.0f + 4096.0f)
                    : 0;
                device->SetRenderState(D3DRS_TEXTUREFACTOR, tf);
            }

            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.verts, 0, mesh.faces);
        }
    };

    if (Configuration.UseSharedMemory) {
        drawLand(visLandShared);
    } else {
        drawLand(visLand);
    }

    // Tear down the detail stage so its texture-transform + stage-1 bind don't
    // leak into later FFP draws this frame (grass / statics share this device
    // state). Restore the identity texture transform and disable stage 1.
    if (texWorldDetail) {
        D3DXMATRIX texIdent;
        D3DXMatrixIdentity(&texIdent);
        device->SetTransform(D3DTS_TEXTURE1, &texIdent);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTexture(1, nullptr);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
}

// Render grass using fixed-function pipeline (non-instanced).
// Each visible grass mesh is drawn individually with its own world transform.
// Uses the same vertex decompression as distant statics since grass shares
// the compressed vertex format (FLOAT16_4 pos, UBYTE4N normal, D3DCOLOR, FLOAT16_2 tc).
void DistantLand::renderGrassFFP() {
    bool useShared = Configuration.UseSharedMemory;
    if (useShared ? visGrassShared.Empty() : visGrass.Empty()) return;

    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(STATIC_FFP_FVF);

    device->SetTransform(D3DTS_VIEW, &mwView);
    device->SetTransform(D3DTS_PROJECTION, &mwProj);

    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);

    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    IDirect3DTexture9* lastTex = nullptr;
    IDirect3DVertexBuffer9* lastVB = nullptr;

    auto drawGrass = [&](auto& vset) {
        // Reusable scratch buffer (see drawStatics): no per-frame heap alloc/free.
        static std::vector<RenderMesh> localMeshes;
        localMeshes.clear();
        vset.Reset();
        while (!vset.AtEnd()) {
            RenderMesh m = vset.Next();
            if (m.vBuffer && m.verts > 0) localMeshes.push_back(m);
        }

        for (const auto& mesh : localMeshes) {

            if (mesh.tex != lastTex) {
                device->SetTexture(0, mesh.tex);
                lastTex = mesh.tex;
            }

            device->SetTransform(D3DTS_WORLD, &mesh.transform);

            IDirect3DVertexBuffer9* ffpVB = getOrCreateStaticFFPBuffer(device, mesh.vBuffer, mesh.verts);
            if (!ffpVB) continue;

            if (ffpVB != lastVB) {
                device->SetStreamSource(0, ffpVB, 0, SIZEOF_STATIC_FFP_VERT);
                device->SetIndices(mesh.iBuffer);
                lastVB = ffpVB;
            }

            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.verts, 0, mesh.faces);
        }
    };

    if (useShared) {
        drawGrass(visGrassShared);
    } else {
        drawGrass(visGrass);
    }

    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
}

// Release FFP vertex buffers
void DistantLand::releaseFFPBuffers() {
    for (auto& pair : staticFFPBuffers) {
        if (pair.second) pair.second->Release();
    }
    staticFFPBuffers.clear();

    for (auto& pair : landFFPBuffers) {
        if (pair.second) pair.second->Release();
    }
    landFFPBuffers.clear();

    // Batched-submission experiment: drop cached Remix mesh handles + the test
    // material so they're recreated against the fresh device (avoids using stale
    // handles after a reset). The Remix-side meshes leak for now — acceptable for
    // an experiment; a real path would DestroyMesh here.
    g_batchMeshCache.clear();
    g_batchMaterialCache.clear();
    g_batchVBFirstFrame.clear();
    g_batchFrameCounter = 0;
}

#endif // MGE_RTX

// Distant-water FFP companion to renderStageWater().
// Draws a flat radial plane at WaterLevel - 1 using the cached vanilla water
// texture and fixed-function pipeline state, so RTX Remix can replace it with
// E-man's translucent water material via the shared texture-pointer hash.
//
// The plane uses vbWaterFFP (XYZ + UV0 stride 20) and shares ibWater so the
// indexing matches the radial mesh built by initWater().  Z-write is disabled
// so vanilla near-water (drawn 1 unit above by Morrowind itself) wins on the
// overlap with no z-fight.  The cached water texture binds to stage 0 so
// Remix sees the same material hash as the engine's near-water draws.
void DistantLand::renderStageWaterFFP() {
    if (!cachedWaterTex || !vbWaterFFP || !waterFFPDecl || !ibWater) return;
    if (!MWBridge::get()->CellHasWeather()) return;

    auto mwBridge = MWBridge::get();

    // Position the plane 1.0 below water level (the radial mesh has w = -1.0
    // baked into the vertices, so the world transform tracks water level).
    float waterLevel = mwBridge->WaterLevel();
    D3DXMATRIX world;
    D3DXMatrixTranslation(&world, eyePos.x, eyePos.y, waterLevel);

    // Save state via state block so we don't clobber MGE's other passes.
    IDirect3DStateBlock9* sb = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &sb);

    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetVertexDeclaration(waterFFPDecl);
    device->SetStreamSource(0, vbWaterFFP, 0, 20);
    device->SetIndices(ibWater);

    device->SetTransform(D3DTS_WORLD, &world);
    device->SetTransform(D3DTS_VIEW, &mwView);
    device->SetTransform(D3DTS_PROJECTION, &mwProj);

    device->SetTexture(0, cachedWaterTex);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FOGENABLE, TRUE);

    // Inner-radius cull (RTX): skip the disc's center fan + inner rings that sit
    // under Morrowind's vanilla near-water. The engine draws its own water plane
    // over this region; leaving the FFP plane there too stacks two translucent
    // water layers under Remix and shows as a dark triangular wedge ("box")
    // around the player. We draw only the outer rings (the genuine distant water
    // past the vanilla draw distance); vanilla water fills everything inside
    // kWaterFFPInnerCull. Computed once -- mesh resolution is fixed at init.
    // Index layout (see initWater): [center fan = resS tris][ring t = resS*2 tris]...
    static int s_ffpStartIndex = -1, s_ffpPrimCount = 0;
    if (s_ffpStartIndex < 0) {
        const bool ripples = (Configuration.MGEFlags & DYNAMIC_RIPPLES) != 0;
        const int resS = ripples ? 150 : 16;
        const int resT = ripples ? 120 : 15;
        const float kWaterFFPInnerCull = 8192.0f;  // ~1 Morrowind cell; vanilla water covers this
        auto ringRadius = [&](int t) -> float {
            if (ripples) {
                if (t + 1 == resT) return 500000.0f;
                float r = float(t) / float(resT);
                return 9600.0f * (0.9f * r * r * r + 0.1f * r);
            }
            return 4096.0f * (1.0f + float(t) * float(t));
        };
        int skipTris = resS;  // center fan (r = 0 .. ring 0)
        for (int t = 1; t < resT; ++t) {
            if (ringRadius(t) <= kWaterFFPInnerCull) {
                skipTris += resS * 2;  // whole ring is inside the cull radius
            } else {
                break;  // first ring crossing the cull radius bridges out to distant water
            }
        }
        s_ffpStartIndex = skipTris * 3;
        s_ffpPrimCount = numWaterTris - skipTris;
        if (s_ffpPrimCount < 0) { s_ffpPrimCount = 0; }
    }

    if (s_ffpPrimCount > 0) {
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, numWaterVerts, s_ffpStartIndex, s_ffpPrimCount);
    }

    if (sb) {
        sb->Apply();
        sb->Release();
    }
}
