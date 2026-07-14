#pragma once

#include "quadtree.h"
#include "ffeshader.h"
#include "mwbridge.h"
#include "specificrender.h"
#include "ipc/client.h"
#include "ipc/dlshare.h"
#include "compositecache.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>



struct MGEShader;

class DistantLand {
public:
    struct DynamicVisGroup {
        enum class DataSource : uint8_t {
            Journal = 1,
            Global = 2,
            UniqueObject = 3
        };
        struct Range {
            int begin, end;
        };

        DataSource source;
        bool enabled;
        const void *gameObject;
        std::string id;
        std::vector<Range> ranges;
        std::vector<QuadTreeMesh*> references;

        void push_back(QuadTreeMesh* mesh) {
            references.push_back(mesh);
        }
    };

    struct RecordedState : RenderedState {
        RecordedState(const RenderedState&);
        ~RecordedState();
        RecordedState(const RecordedState&) = delete;
        RecordedState(RecordedState&&) noexcept;
    };

    static constexpr DWORD fvfWave = D3DFVF_XYZRHW | D3DFVF_TEX2;
    static constexpr int waveTexResolution = 512;
    static constexpr float waveTexWorldRes = 2.5f;
    static constexpr int GrassInstStride = 48;
    static constexpr int MaxGrassElements = 8192;
    static constexpr float kCellSize = 8192.0f;
    static constexpr float kDistantZBias = 5e-6f;
    static constexpr float kDistantNearPlane = 4.0f;
    static constexpr float kMoonTag = 88888.0f;

    static bool ready;
    static bool isRenderCached;
    static bool isPPLActive;
    static int numWaterVerts, numWaterTris;

    static IDirect3DDevice9* device;
    static ID3DXEffect* effect;
    static ID3DXEffect* effectShadow;
    static ID3DXEffect* effectDepth;
    static ID3DXEffectPool* effectPool;
    static IDirect3DVertexDeclaration9* LandDecl;
    static IDirect3DVertexDeclaration9* StaticDecl;
    static IDirect3DVertexDeclaration9* WaterDecl;
    static IDirect3DVertexDeclaration9* GrassDecl;

    static VendorSpecificRendering vsr;

    static IPC::Client ipcClient;
    static std::vector<DynamicVisGroup> dynamicVisGroups;
    static void* lastDistantVisCell;
    static bool isDistantLandLoaded;

    static VisibleSet<StlVector> visLand;
    static VisibleSet<StlVector> visDistant;
    static VisibleSet<StlVector> visGrass;

    static VisibleSet<IpcClientVector> visLandShared;
    static VisibleSet<IpcClientVector> visDistantShared;
    static VisibleSet<IpcClientVector> visGrassShared;
    static VisibleSet<IpcClientVector> visExtraShared;
    static IPC::VecView<IPC::DynVisFlag> dynVisFlagsShared;

    static IPC::VecId visLandSharedId;
    static IPC::VecId visDistantSharedId;
    static IPC::VecId visGrassSharedId;
    static IPC::VecId visExtraSharedId;
    static IPC::VecId dynVisFlagsSharedId;

    static std::vector<RecordedState> recordMW;
    static std::vector<RecordedState> recordSky;
    static std::vector< std::pair<const RenderMesh*, int> > batchedGrass;

    static IDirect3DTexture9* texWorldColour, *texWorldNormals, *texWorldDetail;
    // NEW (additive; Architecture B distant-terrain composite texturing). The single
    // IPC-client-owned resident composite cache. The Texture_Binder (compositebinder.h,
    // used by renderDistantLandFFP and renderDistantLand) reads this to resolve a chunk's
    // per-cell composite, falling back to texWorldColour on a miss. DEPENDENCY FOR TASK 8.5:
    // the client receive/admit/evict side (distantinit.cpp) MUST init() and populate THIS
    // SAME instance (init the device, admit streamed CompositeChunkMsg blobs, evictNotVisible
    // per frame). Until 8.5 wires it, the cache is empty so every lookup() misses and the
    // binder takes the Single_Atlas_Path — preserving stock behaviour (Req 6.1, 6.2).
    static ResidentCompositeCache compositeCache;

    // NEW (additive; Architecture B client streaming, task 8.5). hasCompositeSet mirrors the
    // 64-bit server's Format_Loader verdict (surfaced through the InitLandscape RPC OUT
    // fields). It gates EVERY New_Format-only path: when false (Old_Format / inert / non-IPC),
    // the cache stays empty, no streaming runs, every binder lookup misses, and the distant
    // land is bit-identical to stock (Req 4.6, 6.2). The three shared-vector ids carry the
    // StreamVisibleComposites request/response: the delta channel (newVisible \ resident),
    // the per-cell header channel, and the concatenated-DXT1 byte channel. compositeDefaultEdge
    // is the server-reported baked resolution, used to estimate per-cell budget cost.
    static bool hasCompositeSet;
    static IPC::VecId compositeDeltaSharedId;
    static IPC::VecId compositeHeadersSharedId;
    static IPC::VecId compositeBytesSharedId;
    static IPC::VecView<CompositeCellId> compositeDeltaView;
    static IPC::VecView<CompositeChunkMsg> compositeHeadersView;
    static IPC::VecView<std::uint8_t> compositeBytesView;
    static std::uint32_t compositeDefaultEdge;
    // NEW (additive; Architecture B telemetry, task 11.2). Change-detection signature for the
    // Visible_Cell_Set, so Composite_Telemetry is emitted only WHEN the visible set changes
    // (Req 8.1/8.2/8.4) rather than rewriting the sidecar every frame. compositeVisibleSig is
    // an order-independent hash of the current frame's visible cells (XOR/additive fold, so it
    // is independent of unordered_set iteration order); compositeVisibleSigValid is false until
    // the first New_Format frame emits, forcing that first emit. Both stay untouched on
    // Old_Format / inert frames (streamAndReconcileComposites early-returns), so no sidecar is
    // ever written and the distant land is bit-identical to stock.
    static bool compositeVisibleSigValid;
    static std::uint64_t compositeVisibleSig;
    // Default Texture_Memory_Budget in megabytes (design Req 7.2 / 7.4: 64 MB caps the
    // Draw_Distance=2, 1024^2 DXT1 worst-case resident ring at or below 64 MB).
    static constexpr std::uint32_t kCompositeBudgetMB = 64;

    static IDirect3DTexture9* texDepthFrame;
    static IDirect3DSurface9* surfDepthDepth;
    static IDirect3DTexture9* texDistantBlend;
    static IDirect3DTexture9* texReflection;
    static IDirect3DSurface9* surfReflectionZ;
    static IDirect3DVolumeTexture9* texWater;
    static IDirect3DVertexBuffer9* vbWater;
    static IDirect3DIndexBuffer9* ibWater;
    static IDirect3DVertexBuffer9* vbGrassInstances;

#ifdef MGE_RTX
    // Separate UV-bearing VB+decl for the distant-water FFP pass only.
    // The shared vbWater/WaterDecl are XYZ-only at stride 12 and re-used
    // by depth/shadow/legacy-water paths; we can't widen them without
    // breaking those.  This pair carries XYZ + UV0 (stride 20) so E-man's
    // translucent flipbook material has proper UV gradients to animate
    // across the surface.  Index buffer is shared with vbWater (positions
    // are co-aligned).
    static IDirect3DVertexBuffer9* vbWaterFFP;
    static IDirect3DVertexDeclaration9* waterFFPDecl;
    // Cached pointer to Morrowind's vanilla water stage-0 texture.
    // Captured the first time the engine binds the marked water material,
    // so the distant-water FFP plane re-uses the exact same texture object
    // (and therefore the same Remix material hash) as vanilla near water.
    // Both surfaces then resolve to E-man's translucent water replacement.
    static IDirect3DTexture9* cachedWaterTex;
#endif

    static IDirect3DTexture9* texRain, *texRipples, *texRippleBuffer;
    static IDirect3DSurface9* surfRain, *surfRipples, *surfRippleBuffer;
    static IDirect3DVertexBuffer9* vbWaveSim;

    static IDirect3DTexture9* texShadow, *texSoftShadow;
    static IDirect3DSurface9* surfShadowZ;
    static IDirect3DVertexBuffer9* vbFullFrame, *vbClipCube;

    static D3DXMATRIX mwView, mwProj;
    static D3DXMATRIX smView[2], smProj[2], smViewproj[2];
    static D3DXVECTOR4 eyeVec, eyePos, sunVec, sunPos;
    static float sunVis;
    static RGBVECTOR sunCol, sunAmb, ambCol;
    static RGBVECTOR nearFogCol, horizonCol;
    static RGBVECTOR atmOutscatter, atmInscatter;
    static D3DXVECTOR4 atmSkylightScatter;
    static float fogStart, fogEnd;
    static float fogExpStart, fogExpDivisor;
    static float fogNearStart, fogNearEnd;
    static float nearViewRange;
    static float windScaling, niceWeather;
    static float lightSunMult, lightAmbMult;

    static D3DXHANDLE ehRcpRes, ehShadowRcpRes;
    static D3DXHANDLE ehWorld, ehView, ehProj;
    static D3DXHANDLE ehShadowViewproj;
    static D3DXHANDLE ehVertexBlendState, ehVertexBlendPalette;
    static D3DXHANDLE ehAlphaRef, ehMaterialAlpha;
    static D3DXHANDLE ehHasAlpha, ehHasBones, ehHasVCol;
    static D3DXHANDLE ehTex0, ehTex1, ehTex2, ehTex3, ehTex4, ehTex5;
    static D3DXHANDLE ehEyePos, ehFootPos;
    static D3DXHANDLE ehSunCol, ehSunAmb, ehSunVec, ehSunVecView;
    static D3DXHANDLE ehSkyCol, ehFogColNear, ehFogColFar;
    static D3DXHANDLE ehSunPos, ehSunVis;
    static D3DXHANDLE ehOutscatter, ehInscatter, ehSkyScatterFar;
    static D3DXHANDLE ehFogStart, ehFogRange;
    static D3DXHANDLE ehFogNearStart, ehFogNearRange;
    static D3DXHANDLE ehNearViewRange;
    static D3DXHANDLE ehWindVec;
    static D3DXHANDLE ehNiceWeather;
    static D3DXHANDLE ehTime;
    static D3DXHANDLE ehRippleOrigin;
    static D3DXHANDLE ehWaveHeight;

    static std::function<void(IDirect3DSurface9*)> captureScreenHandler;
    static bool captureScreenWithUI;

    static bool init();
    static bool initIpc();
    static bool initShader();
    static bool initDepth();
    static bool initWater();
    static bool initDynamicWaves();
    static bool initLandscapeClient();
    static bool initLandscape();
    // NEW (additive; Architecture B, task 8.5). Activate IPC-client composite streaming after
    // the InitLandscape RPC reports the server's Format_Loader verdict. On New_Format
    // (hasCompositeSet true) it inits compositeCache on the D3D9 device, sets the default
    // Texture_Memory_Budget, and allocates the three StreamVisibleComposites shared channels.
    // On Old_Format / inert it leaves everything off so the renderer stays on the atlas.
    static void initCompositeStreaming(bool hasComposites, std::uint32_t cellCount, std::uint32_t defaultEdgeTexels);
    // NEW (additive; Architecture B, task 8.5). Per-frame IPC-client composite residency.
    // Derives the Visible_Cell_Set from the SAME parameters the distant-land cull uses
    // (eye position + Draw_Distance * kCellSize viewsphere), requests the delta
    // (newVisible \ resident) via StreamVisibleComposites, admits the streamed CompositeChunkMsg
    // blobs into compositeCache, evicts cells that left the set, and tallies the atlas-served
    // count for telemetry. A no-op unless hasCompositeSet (Old_Format / inert / non-IPC stay
    // bit-identical to stock). Reuses the cull's viewsphere; it does NOT run a second quadtree
    // traversal (the client's RenderMesh cull output carries no per-chunk cell identity).
    static void streamAndReconcileComposites();
    static bool initDistantStaticsClient();
    static bool initShadow();
    static bool initGrass();
    static void loadVisGroupsClient(HANDLE h);
    template<class T, class U>
    static bool loadStaticMeshes(HANDLE h, T& distantStatics, U& distantSubsets);
    template<class T, class U>
    static bool loadDistantStaticsClient(T& distantStatics, U& distantSubsets);
    static bool reloadShaders();
    static void release();

    static void editProjectionZ(D3DMATRIX* m, float zn, float zf);
    static bool selectDistantCell();
    static bool isDistantCell();
    static void resolveDynamicVisGroups();
    static void scanDynamicVisGroups();

    static void setView(const D3DMATRIX* m);
    static void setProjection(D3DMATRIX* proj);
    static void setHorizonColour(const RGBVECTOR& c);
    static void setAmbientColour(const RGBVECTOR& c);
    static void setSunLight(const D3DLIGHT8* s);
    static void setScattering(const RGBVECTOR& out, const RGBVECTOR& in);
    static void adjustFog();
    static bool inspectIndexedPrimitive(int sceneCount, const RenderedState* rs, const FragmentState* frs, LightState* lightrs);

    static void renderSky();
    static void renderStage0();
    static void renderStage1();
    static void renderStage2();
    static void renderStageBlend();
    static void renderStageWater();
#ifdef MGE_RTX
    // Distant-water FFP companion to renderStageWater().
    // Draws a flat radial plane at WaterLevel - 1 using the cached vanilla
    // water texture and fixed-function pipeline state, so Remix can replace
    // it with E-man's water material.  Z-write is disabled so vanilla
    // near-water (1 unit above) naturally takes precedence at the seam.
    static void renderStageWaterFFP();
#endif

    static void setupCommonEffect(const D3DXMATRIX* view,const  D3DXMATRIX* proj);

    static void renderDistantLand(ID3DXEffect* e, const D3DXMATRIX* view, const D3DXMATRIX* proj);
    static void renderDistantLandZ();
    static void cullDistantStatics(const D3DXMATRIX* view, const D3DXMATRIX* proj);
    static void renderDistantStatics();
#ifdef MGE_RTX
    // Fixed-function pipeline rendering for RTX Remix
    static void renderDistantStaticsFFP();
    static void renderDistantLandFFP();
    static void renderGrassFFP();
    static void releaseFFPBuffers();
#endif
    static void cullGrass(const D3DXMATRIX* view, const D3DXMATRIX* proj);
    template<class T>
    static void buildGrassInstanceVB(VisibleSet<T>& grassSet);
    static bool hasVisibleGrass();
    static void renderGrassInst();
    static void renderGrassInstZ();
    static void renderGrassCommon(ID3DXEffect* e);

    static void renderWaterReflection(const D3DXMATRIX* view, const D3DXMATRIX* proj);
    static void renderReflectedSky();
    static void renderReflectedStatics(const D3DXMATRIX* view, const D3DXMATRIX* proj);
    static void clearReflection();
    static void simulateDynamicWaves();
    static void renderWaterPlane();

    static void renderDepth();
    static void renderDepthAdditional();
    static void renderDepthRecorded();

    static void renderShadowMap();
    template<class T>
    static void renderShadowLayerGeneric(MWBridge* mwBridge, int layer, const D3DXMATRIX* inverseCameraProj, D3DXMATRIX* view, D3DXMATRIX* proj, VisibleSet<T>& visible_set);
    static void renderShadowLayer(int layer, float radius, const D3DXMATRIX* inverseCameraProj);
    static void renderShadow();
    static void renderShadowDebug();

    static void postProcess();
    static void updatePostShader(MGEShader* shader);

    static void requestCapture(std::function<void(IDirect3DSurface9*)> handler, bool captureWithUI);
    static void checkCaptureScreenshot(bool isUIDrawn);
    static IDirect3DSurface9* captureScreenshot();
};

class RenderTargetSwitcher {
    IDirect3DSurface9* savedTarget, *savedDepthStencil;
    void init(IDirect3DSurface9* target, IDirect3DSurface9* targetDepthStencil);

public:
    RenderTargetSwitcher(IDirect3DSurface9* target, IDirect3DSurface9* targetDepthStencil);
    RenderTargetSwitcher(IDirect3DTexture9* targetTex, IDirect3DSurface9* targetDepthStencil);
    ~RenderTargetSwitcher();
};
