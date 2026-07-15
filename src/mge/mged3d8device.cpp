
#include "mged3d8device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include "mgeversion.h"
#include "configuration.h"
#include "distantland.h"
#include "mwbridge.h"
#include "statusoverlay.h"
#ifndef MGE_RTX
#include "userhud.h"
#endif // MGE_RTX
#include "videobackground.h"

#ifdef MGE_RTX
#include "rt_anticull.h"
#include "retained_world.h"

static std::atomic<bool> retainedReleasePending{ false };

void waitForRetainedDeviceRelease() {
    while (retainedReleasePending.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

static int sceneCount;
static bool rendertargetNormal, isHUDready;
static bool isMainView, isStencilScene, isAmbientWhite;
static DWORD stencilRef;
static bool stage0Complete, isFrameComplete, isHUDComplete;
static bool isWaterMaterial, waterDrawn, distantWater;

static bool zoomSensSaved;
static float zoomSensX, zoomSensY;
static D3DXMATRIX camEffectsMatrix;
static float crosshairTimeout;

static RenderedState rs;
static FragmentState frs;
static LightState lightrs;

static void initOnLoad();
static bool detectMenu(const D3DMATRIX* m);
static void captureRenderState(D3DRENDERSTATETYPE a, DWORD b);
static void captureFragmentRenderState(DWORD a, D3DTEXTURESTAGESTATETYPE b, DWORD c);
static void captureTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX* b);
static void captureLight(DWORD a, const D3DLIGHT8* b);
static void captureMaterial(const D3DMATERIAL8* a);
static float calcFPS();



MGEProxyDevice::MGEProxyDevice(IDirect3DDevice9* real, Direct3D8* d3d, bool EnableZBufferDiscarding) : Direct3DDevice8(d3d, real, EnableZBufferDiscarding) {
#ifdef MGE_RTX
    // A deferred final release owns the old device until retained destruction succeeds.
    // Do not initialize a replacement against the same global retained manager meanwhile.
    while (retainedReleasePending.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
    // Initialize state here, as the device is released and recreated on fullscreen Alt-Tab
    sceneCount = -1;
    rendertargetNormal = true;
    isHUDready = false;
    isMainView = isStencilScene = isAmbientWhite = stage0Complete = isFrameComplete = isHUDComplete = false;
    stencilRef = 0;
    isWaterMaterial = waterDrawn = false;
    D3DXMatrixIdentity(&camEffectsMatrix);

    Configuration.CameraEffects.zoom = 1.0;
    Configuration.CameraEffects.zoomRate = 0;
    Configuration.CameraEffects.zoomRateTarget = 0;

    // Initialize state recorder to D3D defaults
    memset(&rs, 0, sizeof(rs));
    rs.zWrite = true;
    rs.diffuseMaterial.r = 1.0f;
    rs.diffuseMaterial.g = 1.0f;
    rs.diffuseMaterial.b = 1.0f;
    rs.diffuseMaterial.a = 1.0f;
    rs.cullMode = D3DCULL_CCW;
    rs.useLighting = true;

    rs.matSrcDiffuse = D3DMCS_COLOR1;
    rs.matSrcEmissive = D3DMCS_MATERIAL;

    memset(&frs, 0, sizeof(frs));
    for (FragmentState::Stage* s = &frs.stage[0]; s != &frs.stage[8]; ++s) {
        s->colorOp = D3DTOP_DISABLE;
        s->alphaOp = D3DTOP_DISABLE;
        s->colorArg1 = s->alphaArg1 = D3DTA_TEXTURE;
        s->colorArg2 = s->alphaArg2 = D3DTA_CURRENT;
        s->colorArg0 = s->alphaArg0 = s->resultArg = D3DTA_CURRENT;
    }
    frs.stage[0].colorOp = D3DTOP_MODULATE;
    frs.stage[0].alphaOp = D3DTOP_SELECTARG1;

    lightrs.lights.clear();
    lightrs.active.clear();

    // Store active device in distant land, occurs on startup and after fullscreen alt-tab
    DistantLand::device = ProxyInterface;

    // Patch splash screen minor issues
    D3DVIEWPORT9 vp;
    ProxyInterface->GetViewport(&vp);
    MWBridge::get()->patchSplashScreen(vp.Width, vp.Height);
}

HRESULT _stdcall MGEProxyDevice::Reset(D3DPRESENT_PARAMETERS8* presentationParameters) {
    if (presentationParameters == nullptr) {
        return D3DERR_INVALIDCALL;
    }
#ifdef MGE_RTX
    if (!RetainedWorld::beforeDeviceReset()) {
        return D3DERR_DEVICELOST;
    }
    DistantLand::compositeCache.releaseAll();
    DistantLand::compositeCache.init(nullptr);
#endif
    const HRESULT result = Direct3DDevice8::Reset(presentationParameters);
#ifdef MGE_RTX
    if (SUCCEEDED(result)) {
        DistantLand::compositeCache.init(ProxyInterface);
    }
    RetainedWorld::afterDeviceReset(ProxyInterface, SUCCEEDED(result));
#endif
    return result;
}

// Present - End of MW frame
// MGE end of frame processing
HRESULT _stdcall MGEProxyDevice::Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    auto mwBridge = MWBridge::get();

    // Load Morrowind's dynamic memory pointers
    if (!mwBridge->IsLoaded() && mwBridge->CanLoad()) {
        mwBridge->Load();

        // Apply patch to load distant land before the main menu, and on renderer restart
        mwBridge->patchGameLoading(&initOnLoad);
        // Patch world rendering (on a branch without the water) to split alphas to their own scene
        mwBridge->patchWorldRenderingAccumulation();
        // Disable MW screenshot function to allow MGE to use the same key
        mwBridge->disableScreenshotFunc();
        // Mark water material to allow MGEProxyDevice to detect it
        // Under MGE_RTX we mark for *detection only* (so we can capture the
        // bound water texture for FFP distant-water reuse) — we still let
        // the engine's vanilla water DIP pass through to D3D9 so Remix
        // applies E-man's translucent water material to the near-water plane.
        mwBridge->markWaterNode(99999.0f);
    }

    if (mwBridge->IsLoaded()) {
        if (Configuration.Force3rdPerson && DistantLand::ready) {
            // Set 3rd person camera
            D3DXVECTOR3* camera = mwBridge->PCam3Offset();
            if (camera) {
                camera->x = Configuration.Offset3rdPerson.x;
                camera->y = Configuration.Offset3rdPerson.y;
                camera->z = Configuration.Offset3rdPerson.z;
            }
        }

        if ((Configuration.MGEFlags & CROSSHAIR_AUTOHIDE) && !mwBridge->IsLoadScreen()) {
            // Update crosshair visibility
            float t = mwBridge->simulationTime();

            // Turn on if Morrowind ray cast picks up a target
            if (mwBridge->getPlayerTarget()) {
                crosshairTimeout = t + 1.5f;
            }

            // Turn on short duration if the player requires aim
            if (mwBridge->isPlayerCasting() || mwBridge->isPlayerAimingWeapon()) {
                crosshairTimeout = t + 0.5f;
            }

            // Turn off in menu mode
            if (mwBridge->IsMenu()) {
                crosshairTimeout = t;
            }

            // Allow manual toggle of crosshair to work again from 0.5 seconds after timeout
            if (t < crosshairTimeout + 0.5) {
                mwBridge->SetCrosshairEnabled(t < crosshairTimeout);
            }
        }

        if (Configuration.CameraEffects.zoomRateTarget != 0 && !mwBridge->IsMenu()) {
            // Update zoom controller
            Configuration.CameraEffects.zoomRate += 0.25f * Configuration.CameraEffects.zoomRateTarget * mwBridge->frameTime();
            if (Configuration.CameraEffects.zoomRate / Configuration.CameraEffects.zoomRateTarget > 1.0) {
                Configuration.CameraEffects.zoomRate = Configuration.CameraEffects.zoomRateTarget;
            }

            Configuration.CameraEffects.zoom += Configuration.CameraEffects.zoomRate * mwBridge->frameTime();
            Configuration.CameraEffects.zoom = std::max(1.0f, Configuration.CameraEffects.zoom);
            Configuration.CameraEffects.zoom = std::min(Configuration.CameraEffects.zoom, 8.0f);
        }

        float* mwSens = mwBridge->getMouseSensitivityYX();
        if ((Configuration.MGEFlags & ZOOM_ASPECT) && !mwBridge->IsMenu()) {
            // Adjust sensitivity to accommodate zoom level
            if (!zoomSensSaved) {
                zoomSensY = mwSens[0];
                zoomSensX = mwSens[1];
                zoomSensSaved = true;
            }
            mwSens[0] = zoomSensY / Configuration.CameraEffects.zoom;
            mwSens[1] = zoomSensX / Configuration.CameraEffects.zoom;
        } else if (zoomSensSaved) {
            // Restore unzoomed sensitivity
            mwSens[0] = zoomSensY;
            mwSens[1] = zoomSensX;
            zoomSensSaved = false;
        }

        if (Configuration.CameraEffects.rotateUpdate) {
            Configuration.CameraEffects.rotation += Configuration.CameraEffects.rotationRate * mwBridge->frameTime();
            D3DXMatrixRotationZ(&camEffectsMatrix, Configuration.CameraEffects.rotation);
            if (Configuration.CameraEffects.rotationRate == 0) {
                Configuration.CameraEffects.rotateUpdate = false;
            }
        }
        if (Configuration.CameraEffects.shake) {
            // Update screen shake controller
            Configuration.CameraEffects.shakeMagnitude += Configuration.CameraEffects.shakeAccel * mwBridge->frameTime();
            Configuration.CameraEffects.shakeMagnitude = std::max(0.0f, std::min(100.0f, Configuration.CameraEffects.shakeMagnitude));
            camEffectsMatrix._41 = Configuration.CameraEffects.shakeMagnitude * sin(0.001f*GetTickCount());
        }

        // Main menu background video
        VideoPatch::monitor(ProxyInterface);
    }

    // Reset scene identifiers
    sceneCount = -1;
    rendertargetNormal = true;
    stage0Complete = false;
    waterDrawn = false;
    isFrameComplete = false;
    isHUDComplete = false;

#ifdef MGE_RTX
    // Auto-select RT_AntiCull's reachability range by cell type, then advance its frame
    // counter. CellHasWeather() is true for exterior + behaves-as-exterior cells, false for
    // true interiors — exactly the split RT_AntiCull's two ranges target.
    if (mwBridge->IsLoaded()) {
        RTAntiCull::selectRangeForCell(mwBridge->CellHasWeather());
    }
    RTAntiCull::beginFrame();
#endif

    return Direct3DDevice8::Present(a, b, c, d);
}

// SetRenderTarget
// Remember if MW is rendering to back buffer
HRESULT _stdcall MGEProxyDevice::SetRenderTarget(IDirect3DSurface8* a, IDirect3DSurface8* b) {
    if (a) {
        // Try D3D8-level comparison first
        IDirect3DSurface8* backD3D8 = nullptr;
        Direct3DDevice8::GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backD3D8);
        bool isBack = (a == backD3D8);
        if (backD3D8) backD3D8->Release();

        // Fallback: compare underlying D3D9 surfaces
        if (!isBack) {
            IDirect3DSurface9* back9 = nullptr;
            ProxyInterface->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back9);
            IDirect3DSurface9* target9 = static_cast<Direct3DSurface8*>(a)->GetProxyInterface();
            isBack = (target9 == back9);
            if (back9) back9->Release();
        }

        rendertargetNormal = isBack;
    }

    return Direct3DDevice8::SetRenderTarget(a, b);
}


// BeginScene - Multiple scenes per frame, non-alpha / 2x stencil / post-stencil redraw / alpha / 1st person / UI
// Fogging needs to be set for Morrowind rendering at start of scene
HRESULT _stdcall MGEProxyDevice::BeginScene() {
    auto mwBridge = MWBridge::get();

    HRESULT hr = Direct3DDevice8::BeginScene();
    if (hr != D3D_OK) {
        return hr;
    }

    if (mwBridge->IsLoaded() && rendertargetNormal) {
        if (!isHUDready) {
            // Initialize HUD
            StatusOverlay::init(ProxyInterface);
            StatusOverlay::setStatus(XE_VERSION_STRING);
#ifndef MGE_RTX
            MGEhud::init(ProxyInterface);
#endif // MGE_RTX

            // Set scaling on Morrowind's UI system
            if (Configuration.UIScale != 1.0f) {
                mwBridge->setUIScale(Configuration.UIScale);
            }

            isHUDready = true;
        }

        if (isMainView) {
            // Track scene count here in BeginScene
            // isMainView is not always valid at EndScene if Morrowind draws sunglare
            ++sceneCount;

            // Set any custom FOV and check distant water state
            if (sceneCount == 0) {
                if (Configuration.ScreenFOV > 0) {
                    mwBridge->SetFOV(Configuration.ScreenFOV);
                }
                distantWater = (Configuration.MGEFlags & USE_DISTANT_LAND) || (Configuration.MGEFlags & USE_DISTANT_WATER);
            }
        } else {
            // UI scene, apply post-process if there was anything drawn before it
            // Race menu will render an extra scene past this point
            if (DistantLand::ready && sceneCount > 0 && !isFrameComplete) {
                DistantLand::postProcess();
            }

            // Render user HUD before Morrowind HUD
#ifndef MGE_RTX
            if (isHUDready && !isHUDComplete) {
                MGEhud::draw();
            }
#endif // MGE_RTX

            isFrameComplete = true;
        }
    }

    return D3D_OK;
}

// EndScene - Multiple scenes per frame, non-alpha / 2x stencil / post-stencil redraw / alpha / 1st person / UI
// MGE intercepts first scene to draw distant land before it finishes, others it applies shadows to
HRESULT _stdcall MGEProxyDevice::EndScene() {
    if (DistantLand::ready && rendertargetNormal) {
        if (sceneCount == 0) {
            // Edge case, render distant land even if Morrowind has culled everything
            if (!stage0Complete) {
                DistantLand::renderStage0();
                stage0Complete = true;
            }

            // Opaque features
#ifndef MGE_RTX
            DistantLand::renderStage1();
#else
            // RTX: Render grass via FFP (stage1's shader-based grass is incompatible)
            if (DistantLand::isDistantCell()) {
                DistantLand::cullGrass(&DistantLand::mwView, &DistantLand::mwProj);
                if (Configuration.MGEFlags & USE_GRASS) {
                    DistantLand::renderGrassFFP();
                }
            }
#endif

            // Blend close objects over distant land
            DistantLand::renderStageBlend();

#ifdef MGE_RTX
            // RTX: render the FFP distant-water plane in scene 0, after the
            // close-blend pass.  This binds the cached vanilla water texture
            // so Remix routes both surfaces (near vanilla + distant FFP) to
            // E-man's translucent water replacement via the same material hash.
            if (distantWater) {
                DistantLand::renderStageWaterFFP();
            }
#endif
        } else if (!isFrameComplete) {
            // Everything else except UI
#ifndef MGE_RTX
            DistantLand::renderStage2();
#endif

            // Draw water if the Morrowind water plane doesn't appear in view
#ifndef MGE_RTX
            if (distantWater && !waterDrawn && !isStencilScene) {
                DistantLand::renderStageWater();
                waterDrawn = true;
            }
#endif
        }
    }

    if (isFrameComplete && isHUDready && !isHUDComplete) {
        // Capture post-UI screenshots here
        DistantLand::checkCaptureScreenshot(true);

        // Render status overlay
        StatusOverlay::setFPS(calcFPS());
        StatusOverlay::show(ProxyInterface);

        isHUDComplete = true;
    }

    return Direct3DDevice8::EndScene();
}

// Clear - Occurs at start of frame, and also a z-clear before rendering 1st person and sunglare
HRESULT _stdcall MGEProxyDevice::Clear(DWORD a, const D3DRECT* b, DWORD c, D3DCOLOR d, float e, DWORD f) {
    DistantLand::setHorizonColour(d);
    return Direct3DDevice8::Clear(a, b, c, d, e, f);
}

// SetTransform
// Projection needs modifying to allow room for distant land
HRESULT _stdcall MGEProxyDevice::SetTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX* b) {
    captureTransform(a, b);

    if (rendertargetNormal) {
        if (a == D3DTS_VIEW) {
            // Check for UI view
            isMainView = !detectMenu(b);

            if (isMainView) {
                D3DXMATRIX view = *b;
                view *= camEffectsMatrix;
                return Direct3DDevice8::SetTransform(a, &view);
            }
        } else if (a == D3DTS_PROJECTION) {
            // Only screw with main scene projection
            if (isMainView) {
                D3DXMATRIX proj = *b;
                DistantLand::setProjection(&proj);

                if (Configuration.MGEFlags & ZOOM_ASPECT) {
                    proj._11 *= Configuration.CameraEffects.zoom;
                    proj._22 *= Configuration.CameraEffects.zoom;
                }

                return Direct3DDevice8::SetTransform(a, &proj);
            }
        }
    }

    return Direct3DDevice8::SetTransform(a, b);
}

// SetMaterial
HRESULT _stdcall MGEProxyDevice::SetMaterial(const D3DMATERIAL8* a) {
    captureMaterial(a);
    isWaterMaterial = (a->Power == 99999.0f);

    return Direct3DDevice8::SetMaterial(a);
}

// SetLight
HRESULT _stdcall MGEProxyDevice::SetLight(DWORD a, const D3DLIGHT8* b) {
    captureLight(a, b);

    // Exterior sunlight/interior "sun" appears to always be light 6
    if (a == 6 && DistantLand::ready) {
        DistantLand::setSunLight(b);

#ifdef MGE_RTX
        // Kill the redundant analytical light 6 wherever the path-traced
        // atmosphere owns scene lighting. In atmosphere mode (skyMode == 1) the
        // Atmos SUN and MOONS are injected as real Remix RtDistantLights driven
        // by the atmosphere model (rtx_fork_atmosphere.cpp: g_atmoLights.sun +
        // g_atmoLights.moons[], via LightManager::createExternallyTrackedLight)
        // and illuminate every surface through the standard NEE/RTXDI path --
        // so Bethesda's directional light 6 is a pure duplicate: a SECOND SUN by
        // day and an extra (over-bright) THIRD MOON by night. It also costs a
        // real distant-light NEE sample per bounce.
        //
        // (remixplus-sync 2026-06-21: this used to cite the bespoke
        // evalAtmosphereSunNEE / sampleAtmosphereMoonLight shader path; that path
        // was removed when we adopted Kim's "sun/moon as real distant lights"
        // model. The kill is still correct -- the injected distant lights now do
        // what the NEE did, and the injected-moon radiance mirrors the old moon
        // NEE formula (with the NEE->distant-light /pi conversion), so night
        // brightness is preserved.)
        //
        // Fix: zero its radiance and submit that. createFromDirectional maps
        // D3DLIGHT.Diffuse -> the Remix distant-light colour, and
        // LightManager::addLight (rtx_light_manager.cpp:611) DROPS any light whose
        // radiance is <= 0 in all channels ("light is off"). So a zeroed light 6
        // is culled entirely: no contribution AND no sampling cost (the perf win).
        // setSunLight() above still feeds MGE's own distant-land shading, so
        // nothing is lost there.
        //
        // Gate on CellHasWeather(): exterior + behave-as-exterior interiors
        // (Mournhold, grottos) run the atmosphere, so kill light 6 there. TRUE
        // interiors gate the atmosphere off (syncRemixSky pushes sun -90 + moons
        // disabled), so there light 6 is the legitimate interior fill and must
        // pass through unchanged.
        //
        // (Supersedes the prior night-only moonlight redirect: now that the Atmos
        // moons are confirmed to light surfaces directly, syncing a fake moon
        // light is both redundant and a perf cost.)
        auto mwBridge = MWBridge::get();
        if (b && mwBridge->IsLoaded() && mwBridge->CellHasWeather()) {
            D3DLIGHT8 killed = *b;
            killed.Diffuse  = { 0.0f, 0.0f, 0.0f, 0.0f };
            killed.Ambient  = { 0.0f, 0.0f, 0.0f, 0.0f };
            killed.Specular = { 0.0f, 0.0f, 0.0f, 0.0f };
            return Direct3DDevice8::SetLight(a, &killed);
        }
#endif
    }

    return Direct3DDevice8::SetLight(a, b);
}

// SetRenderState
HRESULT _stdcall MGEProxyDevice::SetRenderState(D3DRENDERSTATETYPE a, DWORD b) {
    captureRenderState(a, b);

    if (a == D3DRS_FOGVERTEXMODE || a == D3DRS_FOGTABLEMODE) {
        return D3D_OK;
    }
    if ((Configuration.MGEFlags & USE_DISTANT_LAND) && (a == D3DRS_FOGSTART || a == D3DRS_FOGEND)) {
        return D3D_OK;
    }
    if (a == D3DRS_STENCILENABLE) {
        isStencilScene = b;
    }
    else if (a == D3DRS_STENCILREF) {
        stencilRef = b;
    }

    // Ambient is used for scene detection
    if (a == D3DRS_AMBIENT) {
        isAmbientWhite = (b == 0xffffffff);

        if (!isAmbientWhite) {
            RGBVECTOR amb = D3DCOLOR(b);
            DistantLand::setAmbientColour(amb);
            lightrs.globalAmbient.r = amb.r;
            lightrs.globalAmbient.g = amb.g;
            lightrs.globalAmbient.b = amb.b;
        }
    }

    return Direct3DDevice8::SetRenderState(a, b);
}

// SetTextureStageState
HRESULT _stdcall MGEProxyDevice::SetTextureStageState(DWORD a, D3DTEXTURESTAGESTATETYPE b, DWORD c) {
    captureFragmentRenderState(a, b, c);

    if (b == D3DTSS_MINFILTER) {
        DWORD filter = (c != D3DTEXF_NONE) ? Configuration.ScaleFilter : D3DTEXF_NONE;
        return ProxyInterface->SetSamplerState(a, D3DSAMP_MINFILTER, filter);
    } else if (b == D3DTSS_MIPFILTER) {
        DWORD filter = (c != D3DTEXF_NONE) ? D3DTEXF_LINEAR : D3DTEXF_NONE;
        return ProxyInterface->SetSamplerState(a, D3DSAMP_MIPFILTER, filter);
    }

    return Direct3DDevice8::SetTextureStageState(a, b, c);
}

// DrawIndexedPrimitive
HRESULT _stdcall MGEProxyDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE a, UINT b, UINT c, UINT d, UINT e) {
    bool isShadowStencil = isStencilScene && stencilRef <= 1;
    if (DistantLand::ready && rendertargetNormal && isMainView && !isShadowStencil) {
        rs.primType = a;
        rs.baseIndex = CurrentBaseVertexIndex;
        rs.minIndex = b;
        rs.vertCount = c;
        rs.startIndex = d;
        rs.primCount = e;

        if (!stage0Complete && !isAmbientWhite) {
            DistantLand::renderStage0();
            stage0Complete = true;
        }

        if (isWaterMaterial) {
#ifndef MGE_RTX
            if (distantWater) {
                if (!waterDrawn) {
                    DistantLand::renderStageWater();
                    waterDrawn = true;
                }
                return D3D_OK;
            }
#else
            // RTX: on the first water DIP per frame, capture the bound stage-0
            // texture so the FFP distant-water plane (drawn in EndScene scene 0
            // of the next frame) can re-bind the same texture pointer and
            // therefore land on the same Remix material hash as vanilla near
            // water.  Morrowind cycles 32 water_NN textures (~25 FPS) but only
            // the first DIP per frame binds the cycling colour texture; later
            // water DIPs bind normal/reflection maps that don't cycle, so the
            // !waterDrawn gate doubles as our once-per-frame guard.
            //
            // We then fall through to D3D9 so the engine's vanilla water draw
            // actually issues — Remix needs the live draw to apply E-man's
            // translucent replacement to the near-water surface.
            if (!waterDrawn) {
                IDirect3DBaseTexture9* baseTex = nullptr;
                if (ProxyInterface->GetTexture(0, &baseTex) == D3D_OK && baseTex) {
                    IDirect3DTexture9* asTex = nullptr;
                    if (baseTex->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&asTex) == S_OK && asTex) {
                        if (DistantLand::cachedWaterTex && DistantLand::cachedWaterTex != asTex) {
                            DistantLand::cachedWaterTex->Release();
                        }
                        DistantLand::cachedWaterTex = asTex;  // owns one AddRef ref
                    }
                    baseTex->Release();
                }
                waterDrawn = true;
            }
#endif
        } else {
            if (!DistantLand::inspectIndexedPrimitive(sceneCount, &rs, &frs, &lightrs)) {
                return D3D_OK;
            }
        }
    }

    return Direct3DDevice8::DrawIndexedPrimitive(a, b, c, d, e);
}

// Release
ULONG _stdcall MGEProxyDevice::Release() {
#ifdef MGE_RTX
    bool clearRetainedReleaseGate = false;
    ULONG currentReferences = ProxyInterface->AddRef();
    currentReferences = ProxyInterface->Release();
    const ULONG trackedReferences = static_cast<ULONG>(
        VertexShaderAndDeclarationCount + PixelShaderHandles.size() + StateBlockTokens.size());
    if (trackedReferences <= currentReferences &&
        currentReferences - trackedReferences == 1 &&
        !RetainedWorld::shutdown()) {
        // Publish the gate before creating the retry worker so replacement
        // CreateDevice calls cannot enter through a construction race.
        retainedReleasePending.store(true, std::memory_order_release);
        try {
            auto retryReady = std::make_shared<std::atomic<bool>>(false);
            std::thread retryThread([this, retryReady]() {
                while (!retryReady->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                do {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } while (!RetainedWorld::shutdown());

                // The worker owns the sole logical device reference, so it can safely
                // release renderer resources before consuming that reference.
                DistantLand::release();
                StatusOverlay::release();
                const ULONG remaining = this->Direct3DDevice8::Release();
                retainedReleasePending.store(false, std::memory_order_release);
                if (remaining != 0) {
                    LOG::logline(
                        "RetainedWorld: deferred device release completed with %u internal references",
                        static_cast<unsigned>(remaining));
                }
            });

            ProxyInterface->AddRef();
            Direct3DDevice8::Release();
            retryThread.detach();
            retryReady->store(true, std::memory_order_release);
            LOG::logline("RetainedWorld: final device ownership transferred to teardown retry worker");
            return 1;
        } catch (...) {
            LOG::logline("RetainedWorld: retry worker unavailable; completing teardown synchronously");
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } while (!RetainedWorld::shutdown());
            clearRetainedReleaseGate = true;
        }
    }
#endif

    ULONG r = Direct3DDevice8::Release();

    if (r == 0) {
        DistantLand::release();
#ifndef MGE_RTX
        MGEhud::release();
#endif // MGE_RTX
        StatusOverlay::release();
    }

#ifdef MGE_RTX
    if (clearRetainedReleaseGate) {
        retainedReleasePending.store(false, std::memory_order_release);
    }
#endif
    return r;
}


// --------------------------------------------------------

void initOnLoad() {
    auto mwBridge = MWBridge::get();

    char buffer[64];
    const char* loadingMessage = *(const char**)mwBridge->getGMSTPointer(602);
    int firstWordLength = 0;

    for (const char *c = loadingMessage; *c; ++c) {
        if (*c == ' ') { break; }
        ++firstWordLength;
    }

    std::snprintf(buffer, sizeof(buffer), "%.*s MGE XE...", firstWordLength, loadingMessage);
    mwBridge->showLoadingBar(buffer, 95.0);

    if (DistantLand::init()) {
        if (Configuration.MGEFlags & USE_DISTANT_LAND) {
            mwBridge->SetViewDistance(7168.0);
        }
    } else {
        Configuration.MGEFlags &= ~USE_DISTANT_LAND;
        StatusOverlay::setStatus("MGE XE serious error condition. Exit Morrowind and check mgeXE.log for details.", StatusOverlay::PriorityError);
    }

    mwBridge->destroyLoadingBar();

    VideoPatch::start(DistantLand::device);
}

bool detectMenu(const D3DMATRIX* m) {
    if (m->_41 != 0.0f || !(m->_42 == 0.0f || m->_42 == -600.0f) || m->_43 != 0.0f) {
        return false;
    }

    if ((m->_11 == 0.0f || m->_11 == 1.0f) && m->_12 == 0.0f && (m->_13 == 0.0f || m->_13 == 1.0f) &&
            m->_21 == 0.0f && (m->_22 == 0.0f || m->_22 == 1.0f) && (m->_23 == 0.0f || m->_23 == 1.0f) &&
            (m->_31 == 0.0f || m->_31 == 1.0f) && (m->_32 == 0.0f || m->_32 == 1.0f) && m->_33 == 0.0f) {
        return true;
    }

    return false;
}

// --------------------------------------------------------
// State recording

HRESULT _stdcall MGEProxyDevice::SetTexture(DWORD a, IDirect3DBaseTexture8* b) {
    if (a == 0) {
        rs.texture = b ? static_cast<Direct3DTexture8*>(b)->GetProxyInterface() : NULL;
    }
    return Direct3DDevice8::SetTexture(a, b);
}

HRESULT _stdcall MGEProxyDevice::SetVertexShader(DWORD a) {
    rs.fvf = a;
    return Direct3DDevice8::SetVertexShader(a);
}

HRESULT _stdcall MGEProxyDevice::SetStreamSource(UINT a, IDirect3DVertexBuffer8* b, UINT c) {
    if (a == 0) {
        // Null-check: NiDX8Renderer::DrawPrimitive can call SetStreamSource(0, NULL, 0)
        // from MWSE-driven renders that walk property-stripped scene-graph clones
        // (e.g. Joy of Painting's OcclusionTester subject mask). Without the check,
        // the static_cast deref crashed at NiDX8Renderer::DrawPrimitive+0x2A4.
        // Mirrors the existing null-aware pattern in SetTexture above.
        rs.vb = b ? static_cast<Direct3DVertexBuffer8*>(b)->GetProxyInterface() : nullptr;
        rs.vbOffset = 0;
        rs.vbStride = c;
    }
    return Direct3DDevice8::SetStreamSource(a, b, c);
}

HRESULT _stdcall MGEProxyDevice::SetIndices(IDirect3DIndexBuffer8* a, UINT b) {
    // Null-check: same defensive guard as SetStreamSource above. Engine code
    // paths that draw through property-stripped clones can issue SetIndices(NULL).
    rs.ib = a ? static_cast<Direct3DIndexBuffer8*>(a)->GetProxyInterface() : nullptr;
    return Direct3DDevice8::SetIndices(a, b);
}

HRESULT _stdcall MGEProxyDevice::LightEnable(DWORD a, BOOL b) {
    if (b) {
        if (std::find(lightrs.active.begin(), lightrs.active.end(), a) == lightrs.active.end()) {
            lightrs.active.push_back(a);
        }
    } else {
        if (std::remove(lightrs.active.begin(), lightrs.active.end(), a) != lightrs.active.end()) {
            lightrs.active.pop_back();
        }
    }
    return Direct3DDevice8::LightEnable(a, b);
}

void captureRenderState(D3DRENDERSTATETYPE a, DWORD b) {
    switch (a) {
    case D3DRS_VERTEXBLEND:
        rs.vertexBlendState = b;
        break;
    case D3DRS_ZWRITEENABLE:
        rs.zWrite = b;
        break;
    case D3DRS_CULLMODE:
        rs.cullMode = b;
        break;
    case D3DRS_ALPHABLENDENABLE:
        rs.blendEnable = (BYTE)b;
        break;
    case D3DRS_SRCBLEND:
        rs.srcBlend = (BYTE)b;
        break;
    case D3DRS_DESTBLEND:
        rs.destBlend = (BYTE)b;
        break;
    case D3DRS_ALPHATESTENABLE:
        rs.alphaTest = (BYTE)b;
        break;
    case D3DRS_ALPHAFUNC:
        rs.alphaFunc = (BYTE)b;
        break;
    case D3DRS_ALPHAREF:
        rs.alphaRef = (BYTE)b;
        break;
    case D3DRS_LIGHTING:
        rs.useLighting = (BYTE)b;
        break;
    case D3DRS_FOGENABLE:
        rs.useFog = (BYTE)b;
        break;
    case D3DRS_DIFFUSEMATERIALSOURCE:
        rs.matSrcDiffuse = (BYTE)b;
        break;
    case D3DRS_EMISSIVEMATERIALSOURCE:
        rs.matSrcEmissive = (BYTE)b;
        break;
    }
}

void captureFragmentRenderState(DWORD a, D3DTEXTURESTAGESTATETYPE b, DWORD c) {
    FragmentState::Stage* s = &frs.stage[a];

    switch (b) {
    case D3DTSS_COLOROP:
        s->colorOp = (BYTE)c;
        break;
    case D3DTSS_COLORARG1:
        s->colorArg1 = (BYTE)c;
        break;
    case D3DTSS_COLORARG2:
        s->colorArg2 = (BYTE)c;
        break;
    case D3DTSS_ALPHAOP:
        s->alphaOp = (BYTE)c;
        break;
    case D3DTSS_ALPHAARG1:
        s->alphaArg1 = (BYTE)c;
        break;
    case D3DTSS_ALPHAARG2:
        s->alphaArg2 = (BYTE)c;
        break;
    case D3DTSS_BUMPENVMAT00:
        s->bumpEnvMat[0][0] = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_BUMPENVMAT01:
        s->bumpEnvMat[0][1] = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_BUMPENVMAT10:
        s->bumpEnvMat[1][0] = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_BUMPENVMAT11:
        s->bumpEnvMat[1][1] = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_TEXCOORDINDEX:
        s->texcoordIndex = c;
        break;
    case D3DTSS_BUMPENVLSCALE:
        s->bumpLumiScale = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_BUMPENVLOFFSET:
        s->bumpLumiBias = reinterpret_cast<float&>(c);
        break;
    case D3DTSS_TEXTURETRANSFORMFLAGS:
        s->texTransformFlags = c;
        break;
    case D3DTSS_COLORARG0:
        s->colorArg0 = (BYTE)c;
        break;
    case D3DTSS_ALPHAARG0:
        s->alphaArg0 = (BYTE)c;
        break;
    case D3DTSS_RESULTARG:
        s->resultArg = (BYTE)c;
        break;
    }
}

void captureTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX* b) {
    switch (a) {
    case D3DTS_WORLDMATRIX(0):
        rs.worldTransforms[0] = *b;
        D3DXMatrixMultiply(&rs.worldViewTransforms[0], static_cast<const D3DXMATRIX*>(b), &rs.viewTransform);
        break;
    case D3DTS_WORLDMATRIX(1):
        rs.worldTransforms[1] = *b;
        D3DXMatrixMultiply(&rs.worldViewTransforms[1], static_cast<const D3DXMATRIX*>(b), &rs.viewTransform);
        break;
    case D3DTS_WORLDMATRIX(2):
        rs.worldTransforms[2] = *b;
        D3DXMatrixMultiply(&rs.worldViewTransforms[2], static_cast<const D3DXMATRIX*>(b), &rs.viewTransform);
        break;
    case D3DTS_WORLDMATRIX(3):
        rs.worldTransforms[3] = *b;
        D3DXMatrixMultiply(&rs.worldViewTransforms[3], static_cast<const D3DXMATRIX*>(b), &rs.viewTransform);
        break;
    case D3DTS_VIEW:
        rs.viewTransform = *b;
        lightrs.lightsTransformed.clear();
        break;
    }
}

void captureLight(DWORD a, const D3DLIGHT8* b) {
    LightState::Light* light = &lightrs.lights[a];

    light->type = b->Type;
    light->diffuse = b->Diffuse;

    if (b->Type == D3DLIGHT_POINT) {
        light->position = b->Position;
        light->falloff.x = b->Attenuation0;
        light->falloff.y = b->Attenuation1;
        light->falloff.z = b->Attenuation2;
    } else {
        D3DXVec3Normalize((D3DXVECTOR3*)&light->position, (D3DXVECTOR3*)&b->Direction);
        light->ambient.x = b->Ambient.r;
        light->ambient.y = b->Ambient.g;
        light->ambient.z = b->Ambient.b;
    }
}

void captureMaterial(const D3DMATERIAL8* a) {
    rs.diffuseMaterial = a->Diffuse;
    frs.material.diffuse = a->Diffuse;
    frs.material.ambient = a->Ambient;
    frs.material.emissive = a->Emissive;
    frs.material.emissive.a = a->Power;
}

// --------------------------------------------------------
// FPS meter

float calcFPS() {
    static int lastMillis, framesSinceUpdate;
    static float fps = 0.0f;

    ++framesSinceUpdate;
    int millis = MWBridge::get()->getFrameBeginMillis();
    int diff = millis - lastMillis;

    if (diff >= 500) {
        fps = 1000.0f * framesSinceUpdate / diff;
        lastMillis = millis;
        framesSinceUpdate = 0;
    } else if (diff < 0) {
        lastMillis = millis;
    }

    return fps;
}
