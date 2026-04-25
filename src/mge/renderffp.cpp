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

// FVF for uncompressed statics: position + normal + diffuse color + texcoord
#define STATIC_FFP_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct StaticFFPVertex {
    float x, y, z;       // Position (12 bytes)
    float nx, ny, nz;    // Normal (12 bytes)
    DWORD color;          // Diffuse color (4 bytes)
    float u, v;           // Texcoord (8 bytes)
};                        // Total: 36 bytes

static const int SIZEOF_STATIC_FFP_VERT = sizeof(StaticFFPVertex);

// FVF for uncompressed land: position + texcoord
#define LAND_FFP_FVF (D3DFVF_XYZ | D3DFVF_TEX1)

struct LandFFPVertex {
    float x, y, z;       // Position (12 bytes)
    float u, v;           // Texcoord (8 bytes)
};                        // Total: 20 bytes

static const int SIZEOF_LAND_FFP_VERT = sizeof(LandFFPVertex);

// Maps from compressed VB → uncompressed FFP VB
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> staticFFPBuffers;
static std::unordered_map<IDirect3DVertexBuffer9*, IDirect3DVertexBuffer9*> landFFPBuffers;

// Half-float to float conversion
static float halfToFloat(unsigned short h) {
    unsigned int sign = (h >> 15) & 1;
    unsigned int exponent = (h >> 10) & 0x1F;
    unsigned int mantissa = h & 0x3FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            // Zero
            unsigned int result = sign << 31;
            return *(float*)&result;
        } else {
            // Denormalized
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                exponent--;
            }
            exponent++;
            mantissa &= ~0x400;
        }
    } else if (exponent == 31) {
        // Inf/NaN
        unsigned int result = (sign << 31) | 0x7F800000 | (mantissa << 13);
        return *(float*)&result;
    }

    exponent = exponent + (127 - 15);
    mantissa = mantissa << 13;
    unsigned int result = (sign << 31) | (exponent << 23) | mantissa;
    return *(float*)&result;
}

// Convert a compressed static vertex buffer to uncompressed FFP format
static IDirect3DVertexBuffer9* getOrCreateStaticFFPBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* compressedVB, int vertCount) {
    auto it = staticFFPBuffers.find(compressedVB);
    if (it != staticFFPBuffers.end()) {
        return it->second;
    }

    // Create uncompressed buffer
    IDirect3DVertexBuffer9* ffpVB = nullptr;
    HRESULT hr = device->CreateVertexBuffer(vertCount * SIZEOF_STATIC_FFP_VERT, D3DUSAGE_WRITEONLY, STATIC_FFP_FVF, D3DPOOL_DEFAULT, &ffpVB, nullptr);
    if (FAILED(hr)) {
        LOG::logline("!! Failed to create FFP vertex buffer for statics");
        return nullptr;
    }

    // Lock both buffers
    void* srcData = nullptr;
    void* dstData = nullptr;
    compressedVB->Lock(0, 0, &srcData, D3DLOCK_READONLY);
    ffpVB->Lock(0, 0, &dstData, 0);

    if (srcData && dstData) {
        const unsigned char* src = (const unsigned char*)srcData;
        StaticFFPVertex* dst = (StaticFFPVertex*)dstData;

        for (int i = 0; i < vertCount; i++) {
            // Position: FLOAT16_4 at offset 0 (8 bytes, 4 half-floats, w is unused)
            const unsigned short* pos16 = (const unsigned short*)(src + 0);
            dst->x = halfToFloat(pos16[0]);
            dst->y = halfToFloat(pos16[1]);
            dst->z = halfToFloat(pos16[2]);

            // Normal: UBYTE4N at offset 8 (4 bytes, normalized)
            const unsigned char* norm = src + 8;
            dst->nx = (norm[0] / 255.0f) * 2.0f - 1.0f;
            dst->ny = (norm[1] / 255.0f) * 2.0f - 1.0f;
            dst->nz = (norm[2] / 255.0f) * 2.0f - 1.0f;

            // Color: D3DCOLOR at offset 12 (4 bytes, BGRA)
            dst->color = *(const DWORD*)(src + 12);

            // TexCoord: FLOAT16_2 at offset 16 (4 bytes, 2 half-floats)
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

// Convert a compressed land vertex buffer to uncompressed FFP format
static IDirect3DVertexBuffer9* getOrCreateLandFFPBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* compressedVB, int vertCount) {
    auto it = landFFPBuffers.find(compressedVB);
    if (it != landFFPBuffers.end()) {
        return it->second;
    }

    IDirect3DVertexBuffer9* ffpVB = nullptr;
    HRESULT hr = device->CreateVertexBuffer(vertCount * SIZEOF_LAND_FFP_VERT, D3DUSAGE_WRITEONLY, LAND_FFP_FVF, D3DPOOL_DEFAULT, &ffpVB, nullptr);
    if (FAILED(hr)) {
        LOG::logline("!! Failed to create FFP vertex buffer for land");
        return nullptr;
    }

    void* srcData = nullptr;
    void* dstData = nullptr;
    compressedVB->Lock(0, 0, &srcData, D3DLOCK_READONLY);
    ffpVB->Lock(0, 0, &dstData, 0);

    if (srcData && dstData) {
        const unsigned char* src = (const unsigned char*)srcData;
        LandFFPVertex* dst = (LandFFPVertex*)dstData;

        for (int i = 0; i < vertCount; i++) {
            // Position: FLOAT3 at offset 0 (12 bytes)
            const float* pos = (const float*)(src + 0);
            dst->x = pos[0];
            dst->y = pos[1];
            dst->z = pos[2];

            // TexCoord: SHORT2N at offset 12 (4 bytes, normalized shorts)
            const short* tc = (const short*)(src + 12);
            dst->u = tc[0] / 32767.0f;
            dst->v = tc[1] / 32767.0f;

            src += 16; // SIZEOFLANDVERT
            dst++;
        }
    }

    compressedVB->Unlock();
    ffpVB->Unlock();

    landFFPBuffers[compressedVB] = ffpVB;
    return ffpVB;
}

// Render distant statics using fixed-function pipeline
void DistantLand::renderDistantStaticsFFP() {
    if (!MWBridge::get()->IsExterior()) {
        float clipAt = nearViewRange - 768.0f;
        D3DXPLANE clipPlane(0, 0, clipAt, -(mwProj._33 * clipAt + mwProj._43));
        device->SetClipPlane(0, clipPlane);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
    }

    // Set up fixed-function pipeline
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(STATIC_FFP_FVF);

    // Set up transforms
    device->SetTransform(D3DTS_VIEW, &mwView);
    D3DXMATRIX distProj = mwProj;
    editProjectionZ(&distProj, kDistantNearPlane - 1e-2f, Configuration.DL.DrawDist * kCellSize);
    device->SetTransform(D3DTS_PROJECTION, &distProj);

    // Enable basic lighting
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);

    // Set up texture stage for diffuse texturing
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    // Alpha test for vegetation
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);

    // Z-buffer
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

    // Render each visible static
    IDirect3DTexture9* lastTex = nullptr;
    IDirect3DVertexBuffer9* lastVB = nullptr;

    const auto& visible = visDistant.visible_set;
    for (const auto* mesh : visible) {
        // Set texture if changed
        if (mesh->tex != lastTex) {
            device->SetTexture(0, mesh->tex);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, mesh->hasAlpha);
            lastTex = mesh->tex;
        }

        // Set world transform
        device->SetTransform(D3DTS_WORLD, &mesh->transform);

        // Get or create uncompressed vertex buffer
        IDirect3DVertexBuffer9* ffpVB = getOrCreateStaticFFPBuffer(device, mesh->vBuffer, mesh->verts);
        if (!ffpVB) continue;

        // Set buffers if changed
        if (ffpVB != lastVB) {
            device->SetStreamSource(0, ffpVB, 0, SIZEOF_STATIC_FFP_VERT);
            device->SetIndices(mesh->iBuffer);
            lastVB = ffpVB;
        }

        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh->verts, 0, mesh->faces);
    }

    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

// Render distant land terrain using fixed-function pipeline
void DistantLand::renderDistantLandFFP() {
    if (!MWBridge::get()->IsExterior()) return;

    D3DXMATRIX distProj = mwProj;
    editProjectionZ(&distProj, kDistantNearPlane - 1e-2f, Configuration.DL.DrawDist * kCellSize);
    D3DXMATRIX viewproj = mwView * distProj;
    D3DXVECTOR4 viewsphere(eyePos.x, eyePos.y, eyePos.z, Configuration.DL.DrawDist * kCellSize);

    // Cull
    ViewFrustum frustum(&viewproj);
    visLand.RemoveAll();
    LandQuadTree.GetVisibleMeshes(frustum, viewsphere, visLand);
    visLand.SortByState();
    visLand.RetainWithTTL();
    visLand.SortByState();

    // Set up fixed-function pipeline
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(LAND_FFP_FVF);

    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &mwView);
    device->SetTransform(D3DTS_PROJECTION, &distProj);

    // Texture: world colour map
    device->SetTexture(0, texWorldColour);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Render each visible land chunk
    IDirect3DVertexBuffer9* lastVB = nullptr;

    const auto& visible = visLand.visible_set;
    for (const auto* mesh : visible) {
        IDirect3DVertexBuffer9* ffpVB = getOrCreateLandFFPBuffer(device, mesh->vBuffer, mesh->verts);
        if (!ffpVB) continue;

        if (ffpVB != lastVB) {
            device->SetStreamSource(0, ffpVB, 0, SIZEOF_LAND_FFP_VERT);
            device->SetIndices(mesh->iBuffer);
            lastVB = ffpVB;
        }

        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh->verts, 0, mesh->faces);
    }
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
}

#endif // MGE_RTX
