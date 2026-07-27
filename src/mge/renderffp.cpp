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

#include "retained_world.h"
#include "msoc_bridge.h"
#include "haze_config.h"      // hazeConfig() — live MCM/env tuning for the aerial-perspective haze
#include "dlcull_config.h"    // dlCullConfig() — live MCM/env tuning for the distant-static near-cull
#include <cstdlib>            // getenv
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
// Remix's FFP geometry path silently rejects a second TEXCOORD set, so atlas and per-cell
// composite UVs cannot coexist in one vertex. Keep two cached one-UV buffers per source mesh:
// one decodes the authored province-atlas UV and one derives the cell-local composite UV.
// The draw selects the buffer whose UV domain matches the bound stage-0 texture.
#define LAND_FFP_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

struct LandFFPVertex {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes) — smooth per-vertex normal decoded from the land vertex
    float u, v;          // Active stage-0 texture coordinate (8 bytes)
};                       // Total: 32 bytes

static const int SIZEOF_LAND_FFP_VERT = sizeof(LandFFPVertex);

// Maps from compressed VB → uncompressed FFP VB.
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> staticFFPBuffers;
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> landFFPBuffers;
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> landAtlasFFPBuffers;

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

static IDirect3DVertexBuffer9* getOrCreateLandFFPBuffer(
    IDirect3DDevice9* device,
    IDirect3DVertexBuffer9* compressedVB,
    int vertCount,
    int cellX,
    int cellY,
    bool useCompositeUV) {
    if (!compressedVB || vertCount <= 0) return nullptr;

    auto& buffers = useCompositeUV ? landFFPBuffers : landAtlasFFPBuffers;
    auto it = buffers.find(compressedVB);
    if (it != buffers.end()) {
        return it->second;
    }

    IDirect3DVertexBuffer9* ffpVB = nullptr;
    HRESULT hr = device->CreateVertexBuffer(vertCount * SIZEOF_LAND_FFP_VERT, D3DUSAGE_WRITEONLY, LAND_FFP_FVF, D3DPOOL_DEFAULT, &ffpVB, nullptr);
    if (FAILED(hr)) {
        LOG::logline("!! Failed to create FFP vertex buffer for land (verts=%d, size=%d, hr=0x%08X)", vertCount, vertCount * SIZEOF_LAND_FFP_VERT, hr);
        return nullptr;
    }

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
            // (Position@0, texCoord@12, Normal@16). Decode xyz exactly like the proven statics path.
            const unsigned char* norm = src + 16;
            dst->nx = (norm[0] / 255.0f) * 2.0f - 1.0f;
            dst->ny = (norm[1] / 255.0f) * 2.0f - 1.0f;
            dst->nz = (norm[2] / 255.0f) * 2.0f - 1.0f;

            if (useCompositeUV) {
                // Exact far-edge coordinates must remain 1.0 rather than wrap to the opposite
                // side of the per-cell composite.
                float cu = (pos[0] - cellMinX) / 8192.0f;
                float cv = (pos[1] - cellMinY) / 8192.0f;
                cu = cu < 0.0f ? 0.0f : (cu > 1.0f ? 1.0f : cu);
                cv = cv < 0.0f ? 0.0f : (cv > 1.0f ? 1.0f : cv);
                dst->u = cu;
                dst->v = 1.0f - cv;
            } else {
                // The generator preserves the original province-atlas SHORT2N UV at offset 12.
                // Use it whenever world.dds is bound; applying cell-local UVs to that atlas is the
                // malformed brown/green blob failure this fallback must avoid.
                const short* tc = (const short*)(src + 12);
                dst->u = tc[0] / 32767.0f;
                dst->v = tc[1] / 32767.0f;
            }

            src += SIZEOFLANDVERT; // 20: Position(12) + texCoord(4) + Normal(4)
            dst++;
        }
    }

    compressedVB->Unlock();
    ffpVB->Unlock();

    buffers[compressedVB] = ffpVB;
    return ffpVB;
}


// FNV-1a 64, used for stable content-derived identities (MSOC occlusion query keys).
static uint64_t fnv1a64(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Render distant statics using fixed-function pipeline
#ifdef MGE_RTX
// Optional extra squared radial cull distance from the live "MGE Distant Cull" MCM
// (mge_dlcull.cfg, polled ~500ms) with MGE_DL_NEARCULL_CELLS env fallback.
// This is independent of the mandatory native near-view ownership handoff below and
// independent of the per-texture suppress list.
static float dlOptionalNearCullDistSq() {
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

    // Static UVs are authored as a tiled domain. Own the complete sampler contract here
    // because the preceding terrain pass clamps stage 0 for per-cell composites.
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

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

    auto drawStatics = [&](auto& vset) {
        // Reusable scratch buffer: retains capacity across frames so the per-frame
        // drain of the (already state-sorted) visible set costs no heap alloc/free.
        // Render is single-threaded; one static per lambda instantiation (shared vs
        // non-shared), only one of which runs per config.
        static std::vector<RenderMesh> localMeshes;
        localMeshes.clear();
        vset.Reset();
#ifdef MGE_RTX
        const float optionalNearCullSq = dlOptionalNearCullDistSq();  // 0 => disabled
#endif
        while (!vset.AtEnd()) {
            RenderMesh m = vset.Next();
            if (!m.vBuffer || m.verts <= 0) continue;
#ifdef MGE_RTX
            // A committed retained cell owns this placement, so reject the
            // legacy copy before near-handoff math and MSOC batch construction.
            if (m.cellValid &&
                RetainedWorld::isCellCommitted(m.cellX, m.cellY)) {
                continue;
            }

            // Remix traces the complete submitted mesh, so ownership must change only after
            // Morrowind's near range no longer intersects the instance's world-space sphere.
            // Malformed bounds fail open to the distant copy rather than hiding geometry.
            const float viewDepth =
                m.boundsCenter.x * mwView._13 +
                m.boundsCenter.y * mwView._23 +
                m.boundsCenter.z * mwView._33 + mwView._43;
            const bool validOwnershipBounds =
                std::isfinite(nearViewRange) &&
                std::isfinite(viewDepth) &&
                std::isfinite(m.boundsRadius) &&
                m.boundsRadius >= 0.0f;
            if (nearViewRange > 0.0f && validOwnershipBounds &&
                viewDepth - m.boundsRadius <= nearViewRange) {
                continue;
            }

            // Scoped per-texture suppress: drop the distant copy of any object whose source
            // texture is on the MCM suppress list (i.e. the trees you've replaced in Remix),
            // so only your replacement renders. This remains independent of both near culls.
            if (dlIsDistantTextureSuppressed(m.tex)) continue;

            // Optional blanket radial extension from the MCM. This can hide the loaded-cell
            // overlap beyond Morrowind's view boundary, but disabling it never disables the
            // mandatory native handoff above.
            if (optionalNearCullSq > 0.0f) {
                const float dx = m.boundsCenter.x - eyePos.x;
                const float dy = m.boundsCenter.y - eyePos.y;
                if (dx * dx + dy * dy < optionalNearCullSq) continue;
            }
#endif
            localMeshes.push_back(m);
        }

        // Query the complete visible-static batch against MSOC's published main-scene
        // mask before any D3D or Remix submission work. The host-provided placement
        // identity is stable across the 32/64-bit IPC boundary and already scopes the
        // worldspace, placement, prototype subset, and source record. Missing identity
        // remains zero so the bridge rejects it and the draw fails open.
        static std::vector<MSOCBridge::SphereQuery> occlusionQueries;
        static std::vector<std::uint8_t> occlusionCull;
        occlusionCull.assign(localMeshes.size(), 0);
        if (MSOCBridge::isSnapshotReady()) {
            occlusionQueries.resize(localMeshes.size());
            constexpr std::uint64_t kDistantStaticOcclusionDomain = 0x4d47455354415449ull;
            for (std::size_t i = 0; i < localMeshes.size(); ++i) {
                const auto& mesh = localMeshes[i];
                std::uint64_t identity = 0;
                if (mesh.retainedPlacementIdentity != 0) {
                    identity = fnv1a64(
                        &kDistantStaticOcclusionDomain,
                        sizeof(kDistantStaticOcclusionDomain));
                    identity = fnv1a64(
                        &mesh.retainedPlacementIdentity,
                        sizeof(mesh.retainedPlacementIdentity),
                        identity);
                    if (identity == 0) {
                        identity = 1;
                    }
                }
                occlusionQueries[i] = {
                    identity,
                    mesh.boundsCenter.x,
                    mesh.boundsCenter.y,
                    mesh.boundsCenter.z,
                    mesh.boundsRadius
                };
            }
            MSOCBridge::classifySpheres(
                occlusionQueries.data(),
                occlusionQueries.size(),
                occlusionCull.data());
        } else {
            occlusionQueries.clear();
        }

        // One-entry cache: the statics set is state-sorted (sortVisibleSet ByState),
        // so identical source VBs are adjacent. Reusing the last resolved FFP VB skips
        // the unordered_map lookup for every mesh in a run after the first.
        IDirect3DVertexBuffer9* lastCompressedVB = nullptr;
        IDirect3DVertexBuffer9* lastResolvedFFP = nullptr;

        for (std::size_t meshIndex = 0; meshIndex < localMeshes.size(); ++meshIndex) {
            const auto& mesh = localMeshes[meshIndex];
            if (occlusionCull[meshIndex]) {
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
void DistantLand::renderDistantLandFFP(bool refreshVisibility) {
    if (!MWBridge::get()->IsExterior()) return;

    D3DXMATRIX distProj = mwProj;
    editProjectionZ(&distProj, kDistantNearPlane, Configuration.DL.DrawDist * kCellSize);
    D3DXVECTOR4 viewsphere(eyePos.x, eyePos.y, eyePos.z, Configuration.DL.DrawDist * kCellSize);

    // Cull
    D3DXMATRIX viewproj = mwView * distProj;
    ViewFrustum frustum(&viewproj);

    if (Configuration.UseSharedMemory) {
        if (refreshVisibility) {
            visLandShared.RemoveAll();
            if (ipcClient.getVisibleMeshes(
                    visLandSharedId, frustum, viewsphere, VIS_LAND)) {
                ipcClient.waitForCompletion();
            }
        }
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

    // Texture_Binder chooses stage 0 per chunk. Filtering is common to both texture domains;
    // the draw loop selects WRAP for the province atlas and CLAMP for a per-cell composite.
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

    // Render each visible land chunk.
    IDirect3DVertexBuffer9* lastVB = nullptr;
    bool samplerAddressValid = false;
    bool lastUsesCompositeUV = false;

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
            if (mesh.cellValid &&
                RetainedWorld::isCellCommitted(mesh.cellX, mesh.cellY)) {
                continue;
            }

            IDirect3DTexture9* groundTex = selectGroundTexture(
                mesh, DistantLand::compositeCache, texWorldColour);
            bool useCompositeUV = groundTex != texWorldColour;

            // A failed composite bind falls back only this chunk. Switching the texture also
            // switches the vertex buffer, so world.dds never receives cell-local coordinates.
            HRESULT hrTex = device->SetTexture(0, groundTex);
            if (FAILED(hrTex) && useCompositeUV) {
                groundTex = texWorldColour;
                useCompositeUV = false;
                hrTex = device->SetTexture(0, groundTex);
            }
            if (FAILED(hrTex)) {
                continue;
            }

            if (!samplerAddressValid || useCompositeUV != lastUsesCompositeUV) {
                const D3DTEXTUREADDRESS address = useCompositeUV
                    ? D3DTADDRESS_CLAMP
                    : D3DTADDRESS_WRAP;
                device->SetSamplerState(0, D3DSAMP_ADDRESSU, address);
                device->SetSamplerState(0, D3DSAMP_ADDRESSV, address);
                lastUsesCompositeUV = useCompositeUV;
                samplerAddressValid = true;
            }

            IDirect3DVertexBuffer9* ffpVB = getOrCreateLandFFPBuffer(
                device, mesh.vBuffer, mesh.verts, mesh.cellX, mesh.cellY,
                useCompositeUV);
            if (!ffpVB) continue;

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
    MSOCBridge::invalidateHistory();
    for (auto& pair : staticFFPBuffers) {
        if (pair.second) pair.second->Release();
    }
    staticFFPBuffers.clear();

    for (auto& pair : landFFPBuffers) {
        if (pair.second) pair.second->Release();
    }
    landFFPBuffers.clear();

    for (auto& pair : landAtlasFFPBuffers) {
        if (pair.second) pair.second->Release();
    }
    landAtlasFFPBuffers.clear();
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
