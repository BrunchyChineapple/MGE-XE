
#include "distantland.h"
#include "distantshader.h"
#include "configuration.h"
#include "mwbridge.h"
#include "proxydx/d3d8header.h"
#include "mge/compositebinder.h"   // selectGroundTexture (Texture_Binder, Architecture B)

#include <algorithm>
#include <vector>



// renderSky - Render atmosphere scattering sky layer and other recorded draw calls on top
void DistantLand::renderSky() {
    // Recorded renders
    const auto& recordSky_const = recordSky;
    const int standardCloudVerts = 65, standardCloudTris = 112;
    const int standardMoonVerts = 4, standardMoonTris = 2;

    // Render sky without clouds first
    effect->BeginPass(PASS_RENDERSKY);
    for (const auto& i : recordSky_const) {
        // Skip clouds
        if (i.texture && i.vertCount == standardCloudVerts && i.primCount == standardCloudTris) {
            continue;
        }

        // Set variables in main effect; variables are shared via effect pool
        effect->SetTexture(ehTex0, i.texture);
        if (i.texture) {
            // Textured object; draw as normal in shader, with exceptions:
            // - Sun/moon billboards do not use mipmaps
            // - Moon shadow cutout (prevents stars shining through moons)
            //   which requires colour to be replaced with atmosphere scattering colour
            bool isBillboard = (i.vertCount == standardMoonVerts && i.primCount == standardMoonTris);
            bool isMoonShadow = i.destBlend == D3DBLEND_INVSRCALPHA && !i.useLighting;

            effect->SetBool(ehHasAlpha, true);
            effect->SetBool(ehHasBones, isBillboard);
            effect->SetBool(ehHasVCol, isMoonShadow);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
            device->SetRenderState(D3DRS_SRCBLEND, i.srcBlend);
            device->SetRenderState(D3DRS_DESTBLEND, i.destBlend);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, 1);
        } else {
            // Sky; perform atmosphere scattering in shader
            effect->SetBool(ehHasAlpha, false);
            effect->SetBool(ehHasVCol, true);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
        }

        effect->SetMatrix(ehWorld, &i.worldTransforms[0]);
        effect->CommitChanges();

        device->SetStreamSource(0, i.vb, i.vbOffset, i.vbStride);
        device->SetIndices(i.ib);
        device->SetFVF(i.fvf);
        device->DrawIndexedPrimitive(i.primType, i.baseIndex, i.minIndex, i.vertCount, i.startIndex, i.primCount);
    }
    effect->EndPass();

    // Render clouds with a separate shader
    effect->BeginPass(PASS_RENDERCLOUDS);
    for (const auto& i : recordSky_const) {
        // Clouds only
        if (!(i.texture && i.vertCount == standardCloudVerts && i.primCount == standardCloudTris)) {
            continue;
        }

        effect->SetTexture(ehTex0, i.texture);
        effect->SetBool(ehHasAlpha, true);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
        device->SetRenderState(D3DRS_SRCBLEND, i.srcBlend);
        device->SetRenderState(D3DRS_DESTBLEND, i.destBlend);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, 1);
        effect->SetMatrix(ehWorld, &i.worldTransforms[0]);
        effect->CommitChanges();

        device->SetStreamSource(0, i.vb, i.vbOffset, i.vbStride);
        device->SetIndices(i.ib);
        device->SetFVF(i.fvf);
        device->DrawIndexedPrimitive(i.primType, i.baseIndex, i.minIndex, i.vertCount, i.startIndex, i.primCount);
    }
    effect->EndPass();
}

void DistantLand::renderDistantLand(ID3DXEffect* e, const D3DXMATRIX* view, const D3DXMATRIX* proj) {
    D3DXMATRIX world, viewproj = (*view) * (*proj);
    D3DXVECTOR4 viewsphere(eyePos.x, eyePos.y, eyePos.z, Configuration.DL.DrawDist * kCellSize);

    // Cull and draw
    ViewFrustum frustum(&viewproj);

    if (Configuration.UseSharedMemory) {
        // kick the operation off early so we can do some additional work while it runs
        visLandShared.RemoveAll();
        ipcClient.getVisibleMeshes(visLandSharedId, frustum, viewsphere, VIS_LAND);
    }

    D3DXMatrixIdentity(&world);
    effect->SetMatrix(ehWorld, &world);

    // Stage 0 (the ground base) is now selected per chunk by the Texture_Binder, so the
    // single pre-loop bind of texWorldColour is gone; the detail stages are unchanged so
    // both render paths still composite the normal/detail layers identically (Req 5.4).
    // texWorldColour remains the per-chunk fallback inside the draw loop below (Req 5.2,
    // 6.1): selectGroundTexture() returns it whenever a cell's Per_Cell_Composite is
    // absent, not yet resident, or over budget, so an unresolved cell renders through the
    // Single_Atlas_Path and stage 0 is never null.
    effect->SetTexture(ehTex0, texWorldColour);
    effect->SetTexture(ehTex1, texWorldNormals);
    effect->SetTexture(ehTex2, texWorldDetail);
    e->CommitChanges();

    if (!Configuration.UseSharedMemory) {
        visLand.RemoveAll();
        DistantLandShare::LandQuadTree.GetVisibleMeshes(frustum, viewsphere, visLand);
#ifdef MGE_RTX
        visLand.SortByState();
        visLand.RetainWithTTL();
        visLand.SortByState();
#endif
    }

    device->SetVertexDeclaration(LandDecl);

    // Per-chunk draw with the symmetric per-chunk stage-0 bind (mirrors the FFP path's
    // drawLand loop so both behave identically, Req 5.4). The single global bind above is
    // replaced by selectGroundTexture(mesh, ...) per chunk: the chunk's resident composite
    // when available/within budget, else the world.dds atlas — never null (Req 5.1, 5.2,
    // 6.1). The mesh/quadtree/cull set built above is reused verbatim; only the stage-0
    // texture selection changes per chunk. A per-chunk bind failure (composite null ->
    // atlas) keeps the loop intact and never blanks the rest of the distant land (Req 6.4).
    //
    // DistantLand::compositeCache is the client's single ResidentCompositeCache instance,
    // shared with the FFP path's renderDistantLandFFP (task 9.5). It is defined/populated by
    // the client residency wave (task 8.5); until then it is an empty cache whose lookup()
    // always misses, so selectGroundTexture() falls back to texWorldColour for every chunk
    // and this path is bit-identical to the stock Single_Atlas_Path (Req 6.2).
    auto drawLand = [&](auto& vset, bool parallelRead) {
        std::vector<RenderMesh> localMeshes;
        if (parallelRead) {
            vset.visible_set.start_read();
        }
        vset.Reset();
        while (!vset.AtEnd()) {
            RenderMesh m = vset.Next();
            if (m.vBuffer && m.verts > 0) localMeshes.push_back(m);
        }
        if (parallelRead) {
            vset.visible_set.end_read();
        }

        IDirect3DVertexBuffer9* lastVB = nullptr;
        for (const auto& mesh : localMeshes) {
            // Texture_Binder: stage-0 ground texture for this chunk, never null (Req 5.1/5.2/6.1).
            effect->SetTexture(ehTex0, selectGroundTexture(mesh, DistantLand::compositeCache, texWorldColour));
            effect->CommitChanges();

            if (lastVB != mesh.vBuffer) {
                device->SetIndices(mesh.iBuffer);
                device->SetStreamSource(0, mesh.vBuffer, 0, SIZEOFLANDVERT);
                lastVB = mesh.vBuffer;
            }

            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.verts, 0, mesh.faces);
        }
    };

    if (Configuration.UseSharedMemory) {
        drawLand(visLandShared, true);
    } else {
        drawLand(visLand, false);
    }
}

void DistantLand::renderDistantLandZ() {
    D3DXMATRIX world;

    D3DXMatrixIdentity(&world);
    effect->SetMatrix(DistantLand::ehWorld, &world);
    effectDepth->CommitChanges();

    // Draw with cached vis set
    device->SetVertexDeclaration(LandDecl);
    if (Configuration.UseSharedMemory) {
        visLandShared.Render(device, SIZEOFLANDVERT);
    } else {
        visLand.Render(device, SIZEOFLANDVERT);
    }
}

void DistantLand::cullDistantStatics(const D3DXMATRIX* view, const D3DXMATRIX* proj) {
    D3DXMATRIX ds_proj = *proj, ds_viewproj;
    D3DXVECTOR4 viewsphere(eyePos.x, eyePos.y, eyePos.z, 0);
#ifdef MGE_RTX
    // RTX: start the distant-static pass exactly at the engine's near-static
    // boundary instead of 768u inside it. The stock -768 slab existed so distant
    // statics could alpha-to-coverage cross-fade in while the near ones fade out;
    // the RTX FFP path (renderDistantStaticsFFP) doesn't use that dissolve, so the
    // slab only double-draws every near static in [nearViewRange-768, nearViewRange]
    // -- visible as "double trees" once a near object is replaced with a detailed
    // mesh, and the source of near->far static popping. Contiguous ranges (near
    // engine draw ends where MGE distant begins) give exactly one static at every
    // distance. Fog covers any hairline seam at the handoff.
    float zn = nearViewRange, zf = zn;
#else
    float zn = nearViewRange - 768.0f, zf = zn;
#endif
    float cullDist = fogEnd;

    if (Configuration.UseSharedMemory) {
        visDistantShared.RemoveAll();
    } else {
        visDistant.RemoveAll();
    }

    zf = std::min(Configuration.DL.NearStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = (*view) * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        if (Configuration.UseSharedMemory) {
            ipcClient.getVisibleMeshes(visDistantSharedId, range_frustum, viewsphere, VIS_NEAR);
        } else {
            DistantLandShare::currentWorldSpace->NearStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
        }
    }

    zf = std::min(Configuration.DL.FarStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = (*view) * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        if (Configuration.UseSharedMemory) {
            ipcClient.getVisibleMeshes(visDistantSharedId, range_frustum, viewsphere, VIS_FAR);
        } else {
            DistantLandShare::currentWorldSpace->FarStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
        }
    }

    zf = std::min(Configuration.DL.VeryFarStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = (*view) * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        if (Configuration.UseSharedMemory) {
            ipcClient.getVisibleMeshes(visDistantSharedId, range_frustum, viewsphere, VIS_VERY_FAR);
        } else {
            DistantLandShare::currentWorldSpace->VeryFarStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
        }
    }

    if (Configuration.UseSharedMemory) {
        ipcClient.sortVisibleSet(visDistantSharedId, VisibleSetSort::ByState);
        ipcClient.waitForCompletion();
    } else {
        visDistant.SortByState();
#ifdef MGE_RTX
        visDistant.RetainWithTTL();
        visDistant.SortByState();
#endif
    }
}

void DistantLand::renderDistantStatics() {
    if (!MWBridge::get()->IsExterior()) {
        // Set clipping to stop large architectural meshes (that don't match exactly)
        // from visible overdrawing and causing z-buffer occlusion
        float clipAt = nearViewRange - 768.0f;
        D3DXPLANE clipPlane(0, 0, clipAt, -(mwProj._33 * clipAt + mwProj._43));
        device->SetClipPlane(0, clipPlane);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
    }

    device->SetVertexDeclaration(StaticDecl);

    if (Configuration.UseSharedMemory) {
        visDistantShared.Render(device, effect, effect, &ehTex0, nullptr, &ehHasVCol, &ehWorld, SIZEOFSTATICVERT);
    } else {
        visDistant.Render(device, effect, effect, &ehTex0, nullptr, &ehHasVCol, &ehWorld, SIZEOFSTATICVERT);
    }

    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
}
